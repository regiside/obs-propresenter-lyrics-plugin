#include <obs-module.h>
#include "obs-tabs-compat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_handle = SOCKET;
static constexpr socket_handle invalid_socket_handle = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_handle = int;
static constexpr socket_handle invalid_socket_handle = -1;
#endif

#include "stage-discovery.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("propresenter-lyrics", "en-US")

static const char *plugin_id = "propresenter_lyrics_source";

static void close_socket(socket_handle socket)
{
	if (socket == invalid_socket_handle)
		return;
#ifdef _WIN32
	closesocket(socket);
#else
	close(socket);
#endif
}

static void socket_startup()
{
#ifdef _WIN32
	static std::once_flag once;
	std::call_once(once, [] {
		WSADATA data;
		WSAStartup(MAKEWORD(2, 2), &data);
	});
#endif
}

static std::string trim(std::string value)
{
	auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}

static std::string escape_json(const std::string &value)
{
	std::ostringstream out;
	for (char ch : value) {
		switch (ch) {
		case '\\':
			out << "\\\\";
			break;
		case '"':
			out << "\\\"";
			break;
		case '\n':
			out << "\\n";
			break;
		case '\r':
			out << "\\r";
			break;
		case '\t':
			out << "\\t";
			break;
		default:
			if (static_cast<unsigned char>(ch) < 0x20)
				out << "\\u00" << std::hex << static_cast<int>(ch);
			else
				out << ch;
		}
	}
	return out.str();
}

static std::string normalize_text(std::string text)
{
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '\r')
			text[i] = '\n';
	}
	std::vector<std::string> lines;
	std::string line;
	std::istringstream in(text);
	while (std::getline(in, line, '\n')) {
		while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
			line.pop_back();
		lines.push_back(line);
	}
	while (!lines.empty() && trim(lines.front()).empty())
		lines.erase(lines.begin());
	while (!lines.empty() && trim(lines.back()).empty())
		lines.pop_back();
	std::ostringstream out;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (i)
			out << '\n';
		out << lines[i];
	}
	return out.str();
}

static std::string preview_text(const std::string &text)
{
	std::string preview = normalize_text(text);
	for (char &ch : preview) {
		if (ch == '\n' || ch == '\r' || ch == '\t')
			ch = ' ';
	}
	if (preview.size() > 180)
		preview = preview.substr(0, 177) + "...";
	return preview.empty() ? "(blank)" : preview;
}

static std::string module_config_file_path(const char *filename)
{
	char *path = obs_module_config_path(filename);
	if (!path)
		return filename;
	std::string out = path;
	bfree(path);
	return out;
}

static std::string shell_quote(const std::string &value)
{
	std::string out = "'";
	for (char ch : value) {
		if (ch == '\'')
			out += "'\\''";
		else
			out += ch;
	}
	out += "'";
	return out;
}

static std::string json_unescape(const std::string &value)
{
	std::ostringstream out;
	for (size_t i = 0; i < value.size(); ++i) {
		char ch = value[i];
		if (ch != '\\' || i + 1 >= value.size()) {
			out << ch;
			continue;
		}
		char escaped = value[++i];
		switch (escaped) {
		case 'n':
			out << '\n';
			break;
		case 'r':
			out << '\r';
			break;
		case 't':
			out << '\t';
			break;
		case '"':
		case '\\':
		case '/':
			out << escaped;
			break;
		default:
			out << escaped;
			break;
		}
	}
	return out.str();
}

static bool find_json_string_after(const std::string &json, size_t start, const std::string &key, std::string &out)
{
	const std::string needle = "\"" + key + "\"";
	size_t key_pos = json.find(needle, start);
	if (key_pos == std::string::npos)
		return false;
	size_t colon = json.find(':', key_pos + needle.size());
	if (colon == std::string::npos)
		return false;
	size_t quote = json.find('"', colon + 1);
	if (quote == std::string::npos)
		return false;
	std::string raw;
	bool escaped = false;
	for (size_t i = quote + 1; i < json.size(); ++i) {
		char ch = json[i];
		if (escaped) {
			raw.push_back('\\');
			raw.push_back(ch);
			escaped = false;
			continue;
		}
		if (ch == '\\') {
			escaped = true;
			continue;
		}
		if (ch == '"') {
			out = json_unescape(raw);
			return true;
		}
		raw.push_back(ch);
	}
	return false;
}

static bool contains_json_string_pair(const std::string &json, size_t start, const std::string &key, const std::string &value)
{
	std::string found;
	return find_json_string_after(json, start, key, found) && found == value;
}

static std::string extract_stage_text(const std::string &json, const std::string &channel)
{
	size_t pos = 0;
	while ((pos = json.find("\"acn\"", pos)) != std::string::npos) {
		if (contains_json_string_pair(json, pos, "acn", channel)) {
			std::string text;
			if (find_json_string_after(json, pos, "txt", text))
				return text;
		}
		++pos;
	}
	return "";
}

static size_t find_matching_char(const std::string &text, size_t open_pos, char open_char, char close_char)
{
	int depth = 0;
	bool in_string = false;
	bool escaped = false;
	for (size_t i = open_pos; i < text.size(); ++i) {
		char ch = text[i];
		if (escaped) {
			escaped = false;
			continue;
		}
		if (ch == '\\' && in_string) {
			escaped = true;
			continue;
		}
		if (ch == '"') {
			in_string = !in_string;
			continue;
		}
		if (in_string)
			continue;
		if (ch == open_char)
			++depth;
		else if (ch == close_char && --depth == 0)
			return i;
	}
	return std::string::npos;
}

static std::string extract_current_layout_uid(const std::string &json)
{
	if (!contains_json_string_pair(json, 0, "acn", "psl"))
		return "";
	std::string uid;
	find_json_string_after(json, 0, "uid", uid);
	return uid;
}

static std::vector<std::string> extract_frame_uids_from_scope(const std::string &json, const std::string &channel)
{
	int type = 1;
	if (channel == "ns")
		type = 2;
	else if (channel == "csn")
		type = 3;
	else if (channel == "nsn")
		type = 4;
	else if (channel == "msg")
		type = 5;

	std::vector<std::string> uids;
	std::string type_needle = "\"typ\":" + std::to_string(type);
	size_t pos = 0;
	while ((pos = json.find(type_needle, pos)) != std::string::npos) {
		size_t object_start = json.rfind('{', pos);
		size_t object_end = json.find('}', pos);
		if (object_start != std::string::npos && object_end != std::string::npos) {
			std::string object = json.substr(object_start, object_end - object_start + 1);
			std::string uid;
			if (find_json_string_after(object, 0, "uid", uid))
				uids.push_back(uid);
		}
		pos += type_needle.size();
	}
	return uids;
}

static std::vector<std::string> extract_frame_uids(const std::string &json, const std::string &channel,
						   const std::string &current_layout_uid)
{
	if (current_layout_uid.empty())
		return extract_frame_uids_from_scope(json, channel);

	std::vector<std::string> uids;
	size_t fme_pos = 0;
	while ((fme_pos = json.find("\"fme\"", fme_pos)) != std::string::npos) {
		size_t object_start = json.rfind('{', fme_pos);
		size_t frames_start = json.find('[', fme_pos);
		if (object_start == std::string::npos || frames_start == std::string::npos) {
			++fme_pos;
			continue;
		}
		size_t frames_end = find_matching_char(json, frames_start, '[', ']');
		size_t object_end = find_matching_char(json, object_start, '{', '}');
		if (frames_end == std::string::npos || object_end == std::string::npos) {
			++fme_pos;
			continue;
		}

		std::string layout_header = json.substr(object_start, fme_pos - object_start);
		std::string layout_uid;
		find_json_string_after(layout_header, 0, "uid", layout_uid);
		if (layout_uid == current_layout_uid) {
			std::string frame_scope = json.substr(frames_start, frames_end - frames_start + 1);
			uids = extract_frame_uids_from_scope(frame_scope, channel);
			break;
		}
		fme_pos = object_end + 1;
	}

	if (uids.empty())
		uids = extract_frame_uids_from_scope(json, channel);
	return uids;
}

static bool extract_frame_value_text(const std::string &json, const std::string &channel, std::string &out)
{
	size_t pos = 0;
	while ((pos = json.find("\"acn\"", pos)) != std::string::npos) {
		std::string acn;
		if (find_json_string_after(json, pos, "acn", acn) && acn == channel) {
			if (find_json_string_after(json, pos, "txt", out))
				return true;
		}
		++pos;
	}
	return false;
}

static std::string extract_http_text(const std::string &json, const std::string &channel)
{
	const char *keys[] = {"text", "slideText", "slide_text", "notes", "slideNotes", "label", "slideLabel"};
	const char *wanted[] = {"current", "next", "current_notes", "next_notes", "message"};
	size_t wanted_index = 0;
	if (channel == "ns")
		wanted_index = 1;
	else if (channel == "csn")
		wanted_index = 2;
	else if (channel == "nsn")
		wanted_index = 3;
	else if (channel == "msg")
		wanted_index = 4;

	size_t scope = json.find(std::string("\"") + wanted[wanted_index] + "\"");
	if (scope == std::string::npos)
		scope = 0;
	for (const char *key : keys) {
		std::string out;
		if (find_json_string_after(json, scope, key, out))
			return out;
	}
	return "";
}

static std::string extract_acn(const std::string &json)
{
	std::string acn;
	if (find_json_string_after(json, 0, "acn", acn))
		return acn;
	return "unknown";
}

struct Style {
	uint32_t text_color = 0xFFFFFFFF;
	int text_opacity = 100;
	std::string font_family = "Arial";
	int font_size = 72;
	std::string font_weight = "700";
	std::string font_style = "normal";
	double letter_spacing = 0.0;
	double line_height = 1.15;
	std::string text_align = "center";
	std::string vertical_align = "middle";
	std::string text_transform = "none";
	int max_lines = 0;
	bool disable_line_wrapping = true;
	int outer_padding_x = 72;
	int outer_padding_y = 56;
	std::string scaling = "shrink_to_fit";
	std::string custom_css;
	bool shadow_enabled = true;
	uint32_t shadow_color = 0xFF000000;
	int shadow_opacity = 70;
	int shadow_x = 0;
	int shadow_y = 3;
	int shadow_blur = 14;
	bool line_background_enabled = false;
	bool line_background_hide_when_empty = false;
	uint32_t line_background_color = 0xFF000000;
	int line_background_opacity = 45;
	int line_background_padding_x = 14;
	int line_background_padding_y = 4;
	int line_background_radius = 2;
	int line_gap = 0;
	bool crossfade_enabled = true;
	int crossfade_ms = 350;
};

static std::string color_to_css(uint32_t color)
{
	uint8_t r = color & 0xFF;
	uint8_t g = (color >> 8) & 0xFF;
	uint8_t b = (color >> 16) & 0xFF;
	char buffer[16];
	snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", r, g, b);
	return buffer;
}

static std::string style_to_json(const Style &style)
{
	std::ostringstream out;
	out << "{";
	out << "\"text_color\":\"" << color_to_css(style.text_color) << "\",";
	out << "\"text_opacity\":" << style.text_opacity << ",";
	out << "\"font_family\":\"" << escape_json(style.font_family) << "\",";
	out << "\"font_size\":" << style.font_size << ",";
	out << "\"font_weight\":\"" << escape_json(style.font_weight) << "\",";
	out << "\"font_style\":\"" << escape_json(style.font_style) << "\",";
	out << "\"letter_spacing\":" << style.letter_spacing << ",";
	out << "\"line_height\":" << style.line_height << ",";
	out << "\"text_align\":\"" << escape_json(style.text_align) << "\",";
	out << "\"vertical_align\":\"" << escape_json(style.vertical_align) << "\",";
	out << "\"text_transform\":\"" << escape_json(style.text_transform) << "\",";
	out << "\"max_lines\":" << style.max_lines << ",";
	out << "\"disable_line_wrapping\":" << (style.disable_line_wrapping ? "true" : "false") << ",";
	out << "\"outer_padding_x\":" << style.outer_padding_x << ",";
	out << "\"outer_padding_y\":" << style.outer_padding_y << ",";
	out << "\"scaling\":\"" << escape_json(style.scaling) << "\",";
	out << "\"custom_css\":\"" << escape_json(style.custom_css) << "\",";
	out << "\"shadow_enabled\":" << (style.shadow_enabled ? "true" : "false") << ",";
	out << "\"shadow_color\":\"" << color_to_css(style.shadow_color) << "\",";
	out << "\"shadow_opacity\":" << style.shadow_opacity << ",";
	out << "\"shadow_x\":" << style.shadow_x << ",";
	out << "\"shadow_y\":" << style.shadow_y << ",";
	out << "\"shadow_blur\":" << style.shadow_blur << ",";
	out << "\"line_background_enabled\":" << (style.line_background_enabled ? "true" : "false") << ",";
	out << "\"line_background_hide_when_empty\":" << (style.line_background_hide_when_empty ? "true" : "false") << ",";
	out << "\"line_background_color\":\"" << color_to_css(style.line_background_color) << "\",";
	out << "\"line_background_opacity\":" << style.line_background_opacity << ",";
	out << "\"line_background_padding_x\":" << style.line_background_padding_x << ",";
	out << "\"line_background_padding_y\":" << style.line_background_padding_y << ",";
	out << "\"line_gap\":" << style.line_gap << ",";
	out << "\"line_background_radius\":" << style.line_background_radius << ",";
	out << "\"crossfade_enabled\":" << (style.crossfade_enabled ? "true" : "false") << ",";
	out << "\"crossfade_ms\":" << style.crossfade_ms;
	out << "}";
	return out.str();
}

struct Settings {
	std::string host = "127.0.0.1";
	std::string service_id;
	int port = 50001;
	std::string password;
	std::string api_mode = "stage_with_http_fallback";
	std::string channel = "cs";
	int poll_interval_ms = 500;
	int overlay_port = 39110;
	uint32_t width = 1920;
	uint32_t height = 1080;
	std::string preset_name = "Default";
	std::string preset_to_apply = "Default";
	Style style;
};

class OverlayState {
public:
	bool setText(const std::string &value)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::string normalized = normalize_text(value);
		if (text_ != normalized) {
			text_ = normalized;
			++revision_;
			return true;
		}
		return false;
	}

	void setStyle(const Style &style)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		style_ = style;
		++revision_;
	}

	std::string json() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::ostringstream out;
		out << "{\"text\":\"" << escape_json(text_) << "\",";
		out << "\"style\":" << style_to_json(style_) << ",";
		out << "\"revision\":" << revision_ << "}";
		return out.str();
	}

private:
	mutable std::mutex mutex_;
	std::string text_;
	Style style_;
	uint64_t revision_ = 1;
};

class LogStore {
public:
	LogStore() : LogStore(module_config_file_path("propresenter-lyrics.log")) {}
	explicit LogStore(std::string path) : path_(std::move(path)) {}
	uint64_t revision() const { return revision_.load(); }

	void add(const std::string &message)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		std::ostringstream line;
		line << timestamp() << "  " << message;
		lines_.push_back(line.str());
		if (lines_.size() > 120)
			lines_.erase(lines_.begin(), lines_.begin() + static_cast<long>(lines_.size() - 120));
		++revision_;
		append_file_locked(line.str());
		blog(LOG_INFO, "[propresenter-lyrics] %s", message.c_str());
	}

	std::string text() const
	{
		static constexpr size_t preview_lines = 14;
		std::lock_guard<std::mutex> lock(mutex_);
		if (lines_.empty())
			return "No log entries yet. Apply the source settings to start connecting.";
		std::ostringstream out;
		size_t start = lines_.size() > preview_lines ? lines_.size() - preview_lines : 0;
		if (start > 0)
			out << "(Showing latest " << preview_lines << " lines. Open the log file for full history.)\n";
		for (size_t i = start; i < lines_.size(); ++i)
			out << lines_[i] << "\n";
		return out.str();
	}

	std::string path() const
	{
		return path_;
	}

private:
	std::atomic<uint64_t> revision_{0};
	static constexpr uintmax_t max_log_bytes = 256 * 1024;
	static constexpr uintmax_t trim_to_bytes = 192 * 1024;

	static std::string timestamp()
	{
		auto now = std::chrono::system_clock::now();
		auto time = std::chrono::system_clock::to_time_t(now);
		std::tm tm = {};
#ifdef _WIN32
		localtime_s(&tm, &time);
#else
		localtime_r(&time, &tm);
#endif
		char buffer[16];
		std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &tm);
		return buffer;
	}

	void append_file_locked(const std::string &line)
	{
		try {
			std::filesystem::path path(path_);
			std::filesystem::path parent = path.parent_path();
			if (!parent.empty())
				std::filesystem::create_directories(parent);
			{
				std::ofstream out(path_, std::ios::app);
				out << line << "\n";
			}
			trim_file_locked();
		} catch (...) {
		}
	}

	void trim_file_locked()
	{
		try {
			if (!std::filesystem::exists(path_) || std::filesystem::file_size(path_) <= max_log_bytes)
				return;

			std::ifstream in(path_, std::ios::binary);
			std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			if (contents.size() <= trim_to_bytes)
				return;

			size_t start = contents.size() - static_cast<size_t>(trim_to_bytes);
			size_t next_line = contents.find('\n', start);
			if (next_line != std::string::npos)
				start = next_line + 1;

			std::ofstream out(path_, std::ios::binary | std::ios::trunc);
			out << contents.substr(start);
		} catch (...) {
		}
	}

	mutable std::mutex mutex_;
	std::vector<std::string> lines_;
	std::string path_;
};

static const char *overlay_html = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<style>
html,body{width:100%;height:100%;margin:0;overflow:hidden;background:transparent}#stage{position:fixed;inset:0;overflow:hidden;background:transparent}.layer{position:absolute;inset:0;box-sizing:border-box;padding:var(--layer-padding,0);opacity:0;transition-property:opacity;transition-duration:var(--crossfade-duration,0ms);transition-timing-function:ease;will-change:opacity}.layer.visible{opacity:1}.content{width:100%;height:100%;box-sizing:border-box;overflow:hidden;display:flex;line-height:var(--line-height,1.15);color:var(--text-color,#fff);opacity:var(--text-opacity,1);font-family:var(--font-family,Arial),sans-serif;font-size:var(--font-size,72px);font-weight:var(--font-weight,700);font-style:var(--font-style,normal);letter-spacing:var(--letter-spacing,0);text-align:var(--text-align,center);text-transform:var(--text-transform,none);text-shadow:var(--text-shadow,none);white-space:pre-wrap;overflow-wrap:anywhere}.content.no-wrap{white-space:pre;overflow-wrap:normal}.content.shrink-to-fit{justify-content:var(--content-justify,center);align-items:var(--content-align,center)}.content:not(.shrink-to-fit){flex-direction:column;justify-content:var(--content-align,center);align-items:var(--content-justify,center)}.content:not(.shrink-to-fit) .line{max-width:100%;min-width:0}.content.shrink-to-fit .textFitted{max-width:100%;white-space:pre-wrap}.content.shrink-to-fit.no-wrap .textFitted{white-space:pre}.has-line-gap .line{display:inline-block}.line:not(:last-child){margin-bottom:var(--line-gap,0px)}.hide-empty-lines .empty-line{display:none}.line{display:inline;box-decoration-break:clone;-webkit-box-decoration-break:clone;border-radius:var(--line-bg-radius,0);padding:var(--line-bg-pad-y,0) var(--line-bg-pad-x,0);background:var(--line-bg,transparent)}
</style></head><body><main id="stage"><section id="a" class="layer visible"><div class="content"></div></section><section id="b" class="layer"><div class="content"></div></section></main>
<script>
(function(root,factory){"use strict";if(typeof define==="function"&&define.amd){define([],factory)}else if(typeof exports==="object"){module.exports=factory()}else{root.textFit=factory()}})(typeof global==="object"?global:this,function(){"use strict";var defaultSettings={alignVert:false,alignHoriz:false,multiLine:false,detectMultiLine:true,minFontSize:6,maxFontSize:80,reProcess:true,widthOnly:false,alignVertWithFlexbox:false};return function textFit(els,options){if(!options)options={};var settings={};for(var key in defaultSettings){if(options.hasOwnProperty(key)){settings[key]=options[key]}else{settings[key]=defaultSettings[key]}}if(typeof els.toArray==="function"){els=els.toArray()}var elType=Object.prototype.toString.call(els);if(elType!=="[object Array]"&&elType!=="[object NodeList]"&&elType!=="[object HTMLCollection]"){els=[els]}for(var i=0;i<els.length;i++){processItem(els[i],settings)}};function processItem(el,settings){if(!isElement(el)||!settings.reProcess&&el.getAttribute("textFitted")){return false}if(!settings.reProcess){el.setAttribute("textFitted",1)}var innerSpan,originalHeight,originalHTML,originalWidth;var low,mid,high;originalHTML=el.innerHTML;originalWidth=innerWidth(el);originalHeight=innerHeight(el);if(!originalWidth||!settings.widthOnly&&!originalHeight){if(!settings.widthOnly)throw new Error("Set a static height and width on the target element "+el.outerHTML+" before using textFit!");else throw new Error("Set a static width on the target element "+el.outerHTML+" before using textFit!")}if(originalHTML.indexOf("textFitted")===-1){innerSpan=document.createElement("span");innerSpan.className="textFitted";innerSpan.style["display"]="inline-block";innerSpan.innerHTML=originalHTML;el.innerHTML="";el.appendChild(innerSpan)}else{innerSpan=el.querySelector("span.textFitted");if(hasClass(innerSpan,"textFitAlignVert")){innerSpan.className=innerSpan.className.replace("textFitAlignVert","");innerSpan.style["height"]="";el.className.replace("textFitAlignVertFlex","")}}if(settings.alignHoriz){el.style["text-align"]="center";innerSpan.style["text-align"]="center"}var multiLine=settings.multiLine;if(settings.detectMultiLine&&!multiLine&&innerSpan.scrollHeight>=parseInt(window.getComputedStyle(innerSpan)["font-size"],10)*2){multiLine=true}if(!multiLine){el.style["white-space"]="nowrap"}low=settings.minFontSize;high=settings.maxFontSize;var size=low;while(low<=high){mid=high+low>>1;innerSpan.style.fontSize=mid+"px";if(innerSpan.scrollWidth<=originalWidth&&(settings.widthOnly||innerSpan.scrollHeight<=originalHeight)){size=mid;low=mid+1}else{high=mid-1}}if(innerSpan.style.fontSize!=size+"px")innerSpan.style.fontSize=size+"px";if(settings.alignVert){addStyleSheet();var height=innerSpan.scrollHeight;if(window.getComputedStyle(el)["position"]==="static"){el.style["position"]="relative"}if(!hasClass(innerSpan,"textFitAlignVert")){innerSpan.className=innerSpan.className+" textFitAlignVert"}innerSpan.style["height"]=height+"px";if(settings.alignVertWithFlexbox&&!hasClass(el,"textFitAlignVertFlex")){el.className=el.className+" textFitAlignVertFlex"}}}function innerHeight(el){var style=window.getComputedStyle(el,null);return el.clientHeight-parseInt(style.getPropertyValue("padding-top"),10)-parseInt(style.getPropertyValue("padding-bottom"),10)}function innerWidth(el){var style=window.getComputedStyle(el,null);return el.clientWidth-parseInt(style.getPropertyValue("padding-left"),10)-parseInt(style.getPropertyValue("padding-right"),10)}function isElement(o){return typeof HTMLElement==="object"?o instanceof HTMLElement:o&&typeof o==="object"&&o!==null&&o.nodeType===1&&typeof o.nodeName==="string"}function hasClass(element,cls){return(" "+element.className+" ").indexOf(" "+cls+" ")>-1}function addStyleSheet(){if(document.getElementById("textFitStyleSheet"))return;var style=[".textFitAlignVert{","position: absolute;","top: 0; right: 0; bottom: 0; left: 0;","margin: auto;","display: flex;","justify-content: center;","flex-direction: column;","}",".textFitAlignVertFlex{","display: flex;","}",".textFitAlignVertFlex .textFitAlignVert{","position: static;","}"].join("");var css=document.createElement("style");css.type="text/css";css.id="textFitStyleSheet";css.innerHTML=style;document.body.appendChild(css)}});
const layers=[document.getElementById("a"),document.getElementById("b")];let active=0,lastText=null,lastStyle="",lastRev=0,style={};
function color(hex,op){const h=String(hex||"#000").replace("#","").padEnd(6,"0").slice(0,6),v=parseInt(h,16);return `rgba(${(v>>16)&255},${(v>>8)&255},${v&255},${Math.max(0,Math.min(100,Number(op||0)))/100})`}
function hAlign(v){return v==="left"?"flex-start":v==="right"?"flex-end":"center"}function vAlign(v){return v==="top"?"flex-start":v==="bottom"?"flex-end":"center"}function scaling(){return style.scaling==="shrink_to_fit"?"shrink_to_fit":"none"}function maxFontSize(){return Math.max(6,Number(style.font_size||72))}
function applyCustomCss(css){let el=document.getElementById("custom-css");if(!el){el=document.createElement("style");el.id="custom-css";document.body.appendChild(el)}el.textContent=css||""}
function applyStyle(s){style=s||{};document.documentElement.classList.toggle("hide-empty-lines",!!style.line_background_hide_when_empty);const gap=Math.max(0,Math.min(1000,Number(style.line_gap)||0));document.documentElement.classList.toggle("has-line-gap",gap>0);const r=document.documentElement.style;r.setProperty("--line-gap",`${gap}px`);r.setProperty("--text-color",style.text_color||"#fff");r.setProperty("--text-opacity",Math.max(0,Math.min(100,Number(style.text_opacity??100)))/100);r.setProperty("--font-family",JSON.stringify(style.font_family||"Arial"));r.setProperty("--font-size",`${maxFontSize()}px`);r.setProperty("--font-weight",style.font_weight||"700");r.setProperty("--font-style",style.font_style||"normal");r.setProperty("--letter-spacing",`${Number(style.letter_spacing||0)}px`);r.setProperty("--line-height",Number(style.line_height||1.15));r.setProperty("--text-align",style.text_align||"center");r.setProperty("--content-justify",hAlign(style.text_align));r.setProperty("--content-align",vAlign(style.vertical_align));r.setProperty("--text-transform",style.text_transform||"none");r.setProperty("--layer-padding",`${Number(style.outer_padding_y||0)}px ${Number(style.outer_padding_x||0)}px`);r.setProperty("--crossfade-duration",`${style.crossfade_enabled?Number(style.crossfade_ms||0):0}ms`);r.setProperty("--line-bg-radius",`${Number(style.line_background_radius||0)}px`);r.setProperty("--line-bg-pad-x",`${Number(style.line_background_padding_x||0)}px`);r.setProperty("--line-bg-pad-y",`${Number(style.line_background_padding_y||0)}px`);r.setProperty("--line-bg",style.line_background_enabled?color(style.line_background_color,style.line_background_opacity):"transparent");r.setProperty("--text-shadow",style.shadow_enabled?`${Number(style.shadow_x||0)}px ${Number(style.shadow_y||0)}px ${Number(style.shadow_blur||0)}px ${color(style.shadow_color,style.shadow_opacity)}`:"none");applyCustomCss(style.custom_css||"");requestAnimationFrame(fitAll)}
function line(t){const s=document.createElement("span");s.className="line";s.classList.toggle("empty-line",!String(t).trim());s.textContent=t||"\u00a0";return s}
function setText(layer,text){const c=layer.querySelector(".content");c.replaceChildren();let lines=String(text||"").replace(/\r\n?/g,"\n").split("\n"),max=Number(style.max_lines||0),needsBreaks=scaling()==="shrink_to_fit";if(max>0&&lines.length>max){lines=lines.slice(0,max);lines[max-1]+="..."}let hasContent=false;lines.forEach((x,i)=>{const empty=!x.trim();if(i&&needsBreaks){const br=document.createElement("br");br.classList.toggle("empty-line",empty||!hasContent);c.appendChild(br)}c.appendChild(line(x));if(!empty)hasContent=true});fit(layer)}
function showText(text){if(text===lastText)return;lastText=text;const next=active?0:1;setText(layers[next],text);layers[next].classList.add("visible");layers[active].classList.remove("visible");active=next}
function fit(layer){const c=layer.querySelector(".content");const shrink=scaling()==="shrink_to_fit";c.classList.toggle("shrink-to-fit",shrink);c.classList.toggle("no-wrap",!!style.disable_line_wrapping);c.style.fontSize=`${maxFontSize()}px`;if(!shrink)return;requestAnimationFrame(()=>{try{textFit(c,{alignVert:false,alignHoriz:false,multiLine:true,detectMultiLine:false,minFontSize:6,maxFontSize:maxFontSize(),reProcess:true,widthOnly:false})}catch(e){}})}
function fitAll(){for(const l of layers)fit(l)}async function poll(){try{const res=await fetch(`/state?t=${Date.now()}`,{cache:"no-store"}),st=await res.json(),ss=JSON.stringify(st.style||{});if(ss!==lastStyle){lastStyle=ss;applyStyle(st.style||{});setText(layers[active],lastText||st.text||"")}if(st.revision!==lastRev){lastRev=st.revision;showText(st.text||"")}}catch(e){}setTimeout(poll,120)}window.addEventListener("resize",fitAll);poll();
</script></body></html>)HTML";

class OverlayServer {
public:
	explicit OverlayServer(OverlayState &state) : state_(state) {}
	~OverlayServer() { stop(); }

	bool start(int port)
	{
		if (running_ && port_ == port)
			return true;
		stop();
		socket_startup();
		listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listen_socket_ == invalid_socket_handle)
			return false;

		int yes = 1;
		setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));

		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(static_cast<uint16_t>(port));
		if (bind(listen_socket_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
		    listen(listen_socket_, 8) != 0) {
			close_socket(listen_socket_);
			listen_socket_ = invalid_socket_handle;
			return false;
		}
		port_ = port;
		running_ = true;
		thread_ = std::thread([this] { run(); });
		return true;
	}

	void stop()
	{
		running_ = false;
		close_socket(listen_socket_);
		listen_socket_ = invalid_socket_handle;
		if (thread_.joinable())
			thread_.join();
	}

	std::string url() const
	{
		return "http://127.0.0.1:" + std::to_string(port_) + "/overlay";
	}

private:
	void run()
	{
		while (running_) {
			socket_handle client = accept(listen_socket_, nullptr, nullptr);
			if (client == invalid_socket_handle)
				continue;
			std::thread(&OverlayServer::handle, this, client).detach();
		}
	}

	void handle(socket_handle client)
	{
		char buffer[4096] = {};
		int received = recv(client, buffer, sizeof(buffer) - 1, 0);
		if (received <= 0) {
			close_socket(client);
			return;
		}
		std::string request(buffer, static_cast<size_t>(received));
		std::string body;
		std::string type;
		if (request.rfind("GET /state", 0) == 0) {
			body = state_.json();
			type = "application/json; charset=utf-8";
		} else if (request.rfind("GET /overlay", 0) == 0 || request.rfind("GET / ", 0) == 0) {
			body = overlay_html;
			type = "text/html; charset=utf-8";
		} else {
			body = "Not found";
			type = "text/plain; charset=utf-8";
		}
		std::ostringstream response;
		response << "HTTP/1.1 " << (body == "Not found" ? "404 Not Found" : "200 OK") << "\r\n";
		response << "Content-Type: " << type << "\r\n";
		response << "Cache-Control: no-store\r\n";
		response << "Access-Control-Allow-Origin: *\r\n";
		response << "Content-Length: " << body.size() << "\r\n\r\n";
		response << body;
		std::string out = response.str();
		send(client, out.data(), static_cast<int>(out.size()), 0);
		close_socket(client);
	}

	OverlayState &state_;
	std::atomic<bool> running_ = false;
	socket_handle listen_socket_ = invalid_socket_handle;
	int port_ = 39110;
	std::thread thread_;
};

class StageDisplayClient {
public:
	using TextCallback = std::function<void(const std::string &)>;
	using LogCallback = std::function<void(const std::string &)>;
	~StageDisplayClient() { stop(); }

	// OBS only publishes settings here; it never joins a connecting worker.
	void start(Settings settings, TextCallback callback, LogCallback log_callback)
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		pending_settings_ = std::move(settings);
		pending_callback_ = std::move(callback);
		pending_log_callback_ = std::move(log_callback);
		pending_ = true;
		stopped_ = true;
		if (!thread_.joinable()) {
			quitting_ = false;
			thread_ = std::thread([this] { worker(); });
		}
		control_changed_.notify_all();
	}

	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(control_mutex_);
			quitting_ = true;
			stopped_ = true;
		}
		control_changed_.notify_all();
		if (thread_.joinable()) thread_.join();
	}

private:
	void log(const std::string &message)
	{
		if (log_callback_)
			log_callback_(message);
	}

	using Clock = std::chrono::steady_clock;

	void pause(std::chrono::milliseconds duration)
	{
		std::unique_lock<std::mutex> lock(control_mutex_);
		control_changed_.wait_for(lock, duration, [this] { return stopped_.load(); });
	}

	void worker()
	{
		for (;;) {
			{
				std::unique_lock<std::mutex> lock(control_mutex_);
				control_changed_.wait(lock, [this] { return quitting_ || pending_; });
				if (quitting_) return;
				settings_ = std::move(pending_settings_);
				callback_ = std::move(pending_callback_);
				log_callback_ = std::move(pending_log_callback_);
				pending_ = false;
				stopped_ = false;
			}
			run();
			// Only this thread owns and closes the network sockets.
			close_socket(socket_);
			socket_ = invalid_socket_handle;
		}
	}

	bool wait_socket(socket_handle socket, bool writing, Clock::time_point deadline)
	{
#ifndef _WIN32
		if (socket < 0 || socket >= FD_SETSIZE) return false;
#endif
		while (!stopped_ && Clock::now() < deadline) {
			fd_set ready;
			FD_ZERO(&ready);
			FD_SET(socket, &ready);
			timeval timeout{0, 50000};
			int result = select(static_cast<int>(socket + 1), writing ? nullptr : &ready,
				writing ? &ready : nullptr, nullptr, &timeout);
			if (result > 0) return !stopped_;
			if (result < 0) return false;
		}
		return false;
	}

	static bool would_block()
	{
#ifdef _WIN32
		int error = WSAGetLastError();
		return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINTR;
#else
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
#endif
	}

	int receive(socket_handle socket, char *data, size_t size, Clock::time_point deadline = Clock::time_point::max())
	{
		while (wait_socket(socket, false, deadline)) {
			int result = recv(socket, data, static_cast<int>(size), 0);
			if (result >= 0 || !would_block()) return result;
		}
		return -1;
	}

	bool send_all(socket_handle socket, const char *data, size_t size)
	{
		auto deadline = Clock::now() + std::chrono::seconds(2);
		while (size && wait_socket(socket, true, deadline)) {
#ifdef MSG_NOSIGNAL
			int sent = send(socket, data, static_cast<int>(size), MSG_NOSIGNAL);
#else
			int sent = send(socket, data, static_cast<int>(size), 0);
#endif
			if (sent < 0 && would_block()) continue;
			if (sent <= 0) return false;
			data += sent;
			size -= static_cast<size_t>(sent);
		}
		return size == 0;
	}

	socket_handle connect_tcp(const std::string &host, int port)
	{
		socket_startup();
		auto addresses = stage_discovery::addresses(host, port, stopped_);
		socket_handle out = invalid_socket_handle;
		for (const auto &address : addresses) {
			if (stopped_) break;
			out = socket(address.ss_family, SOCK_STREAM, IPPROTO_TCP);
			if (out == invalid_socket_handle) continue;
#ifdef _WIN32
			u_long nonblocking = 1;
			bool configured = ioctlsocket(out, FIONBIO, &nonblocking) == 0;
#else
			int flags = fcntl(out, F_GETFL, 0);
			bool configured = flags >= 0 && fcntl(out, F_SETFL, flags | O_NONBLOCK) == 0;
#ifdef SO_NOSIGPIPE
			int yes = 1;
			setsockopt(out, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
#endif
			int length = address.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
			if (configured) {
				int result = connect(out, reinterpret_cast<const sockaddr *>(&address), length);
				if (result == 0) return out;
				if (would_block() && wait_socket(out, true, Clock::now() + std::chrono::seconds(2))) {
					int error = 0;
#ifdef _WIN32
					int error_size = sizeof(error);
#else
					socklen_t error_size = sizeof(error);
#endif
					if (getsockopt(out, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &error_size) == 0 && !error)
						return out;
				}
			}
			close_socket(out);
			out = invalid_socket_handle;
		}
		if (!stopped_) log("TCP connection failed to " + host + ":" + std::to_string(port));
		return invalid_socket_handle;
	}

	bool read_exact(socket_handle socket, void *data, size_t bytes)
	{
		char *cursor = static_cast<char *>(data);
		size_t done = 0;
		while (done < bytes && !stopped_) {
			int got = receive(socket, cursor + done, bytes - done);
			if (got <= 0)
				return false;
			done += static_cast<size_t>(got);
		}
		return done == bytes;
	}

	bool websocket_handshake()
	{
		socket_ = connect_tcp(settings_.host, settings_.port);
		if (socket_ == invalid_socket_handle)
			return false;
		log("Opening Stage Display WebSocket to " + settings_.host + ":" + std::to_string(settings_.port));
		std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
		std::ostringstream request;
		request << "GET /stagedisplay HTTP/1.1\r\n";
		request << "Host: " << settings_.host << ":" << settings_.port << "\r\n";
		request << "Upgrade: websocket\r\nConnection: Upgrade\r\n";
		request << "Sec-WebSocket-Key: " << key << "\r\n";
		request << "Sec-WebSocket-Version: 13\r\n\r\n";
		std::string raw = request.str();
		if (!send_all(socket_, raw.data(), raw.size())) return false;
		auto deadline = Clock::now() + std::chrono::seconds(3);

		std::string response;
		char ch = 0;
		while (response.find("\r\n\r\n") == std::string::npos && response.size() < 65536) {
			if (receive(socket_, &ch, 1, deadline) != 1)
				return false;
			response.push_back(ch);
		}
		bool ok = response.find(" 101 ") != std::string::npos;
		log(ok ? "Stage Display WebSocket connected." : "Stage Display WebSocket handshake failed.");
		return ok;
	}

	void send_ws_frame(const std::string &payload, uint8_t opcode)
	{
		std::vector<uint8_t> frame;
		frame.push_back(static_cast<uint8_t>(0x80 | opcode));
		size_t length = payload.size();
		if (length < 126) {
			frame.push_back(static_cast<uint8_t>(0x80 | length));
		} else if (length < 65536) {
			frame.push_back(0x80 | 126);
			frame.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
			frame.push_back(static_cast<uint8_t>(length & 0xFF));
		} else {
			frame.push_back(0x80 | 127);
			for (int i = 7; i >= 0; --i)
				frame.push_back(static_cast<uint8_t>((length >> (i * 8)) & 0xFF));
		}
		uint8_t mask[4] = {0x13, 0x37, 0x53, 0x90};
		frame.insert(frame.end(), mask, mask + 4);
		for (size_t i = 0; i < payload.size(); ++i)
			frame.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
		send_all(socket_, reinterpret_cast<const char *>(frame.data()), frame.size());
	}

	void send_ws_text(const std::string &payload)
	{
		send_ws_frame(payload, 0x1);
	}

	bool read_ws_text(std::string &message)
	{
		std::string accumulated;
		bool accumulating = false;

		while (!stopped_) {
			uint8_t first[2];
			if (!read_exact(socket_, first, 2))
				return false;
			bool fin = (first[0] & 0x80) != 0;
			uint8_t opcode = first[0] & 0x0F;
			uint64_t length = first[1] & 0x7F;
			if (length == 126) {
				uint8_t ext[2];
				if (!read_exact(socket_, ext, 2))
					return false;
				length = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
			} else if (length == 127) {
				uint8_t ext[8];
				if (!read_exact(socket_, ext, 8))
					return false;
				length = 0;
				for (uint8_t byte : ext)
					length = (length << 8) | byte;
			}
			uint8_t mask[4] = {};
			bool masked = (first[1] & 0x80) != 0;
			if (masked && !read_exact(socket_, mask, 4))
				return false;
			std::string payload(length, '\0');
			if (length && !read_exact(socket_, payload.data(), static_cast<size_t>(length)))
				return false;
			if (masked) {
				for (size_t i = 0; i < payload.size(); ++i)
					payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
			}

			if (opcode == 0x8)
				return false;
			if (opcode == 0x9) {
				send_ws_frame(payload, 0xA);
				continue;
			}
			if (opcode == 0x1) {
				if (fin) {
					message = payload;
					return true;
				}
				accumulated = payload;
				accumulating = true;
				continue;
			}
			if (opcode == 0x0 && accumulating) {
				accumulated += payload;
				if (fin) {
					message = accumulated;
					return true;
				}
			}
		}
		return false;
	}

	std::string http_get_slide()
	{
		socket_handle socket = connect_tcp(settings_.host, settings_.port);
		if (socket == invalid_socket_handle)
			return "";
		std::ostringstream request;
		request << "GET /v1/status/slide HTTP/1.1\r\nHost: " << settings_.host << ":" << settings_.port
			<< "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
		std::string raw = request.str();
		if (!send_all(socket, raw.data(), raw.size())) {
			close_socket(socket);
			return "";
		}
		auto deadline = Clock::now() + std::chrono::seconds(3);
		std::string response;
		char buffer[4096];
		int got = 0;
		while (response.size() < 1024 * 1024 && (got = receive(socket, buffer, sizeof(buffer), deadline)) > 0)
			response.append(buffer, static_cast<size_t>(got));
		close_socket(socket);
		if (got < 0 || stopped_ || response.size() >= 1024 * 1024) return "";
		size_t split = response.find("\r\n\r\n");
		return split == std::string::npos ? response : response.substr(split + 4);
	}

	void run_http_poll_for(std::chrono::milliseconds duration)
	{
		auto until = std::chrono::steady_clock::now() + duration;
		while (!stopped_ && std::chrono::steady_clock::now() < until) {
			std::string body = http_get_slide();
			std::string text = extract_http_text(body, settings_.channel);
			if (!text.empty())
				callback_(text);
			pause(std::chrono::milliseconds(std::max(150, settings_.poll_interval_ms)));
		}
	}

	bool seed_text_from_http_status(const std::string &reason)
	{
		std::string body = http_get_slide();
		std::string text = extract_http_text(body, settings_.channel);
		if (text.empty()) {
			log(reason + " HTTP status seed did not return slide text.");
			return false;
		}
		log(reason + " HTTP status seed received text: " + preview_text(text));
		callback_(text);
		return true;
	}

	void run_stage_session()
	{
		log("Connecting to ProPresenter Stage Display at " + settings_.host + ":" + std::to_string(settings_.port));
		if (!websocket_handshake()) {
			close_socket(socket_);
			socket_ = invalid_socket_handle;
			return;
		}
		send_ws_text("{\"acn\":\"ath\",\"ptl\":610,\"pwd\":\"" + escape_json(settings_.password) + "\"}");
		log("Stage Display authentication sent.");
		std::set<std::string> requested;
		std::string current_layout_uid;
		bool tried_http_seed = false;
		auto last_layout_request = std::chrono::steady_clock::now();
		while (!stopped_) {
			if (std::chrono::steady_clock::now() - last_layout_request > std::chrono::seconds(20)) {
				send_ws_text("{\"acn\":\"psl\"}");
				send_ws_text("{\"acn\":\"asl\"}");
				last_layout_request = std::chrono::steady_clock::now();
			}
			std::string message;
			if (!read_ws_text(message))
				break;
			if (message.empty())
				continue;
			std::string acn = extract_acn(message);
			if (acn != "sys" && acn != "tmr")
				log("Received Stage Display message '" + acn + "' (" + std::to_string(message.size()) + " bytes).");
			if (message.find("\"acn\":\"ath\"") != std::string::npos) {
				if (message.find("\"ath\":false") != std::string::npos)
					log("Stage Display authentication failed. Check the ProPresenter password.");
				else {
					log("Stage Display authentication accepted.");
					send_ws_text("{\"acn\":\"psl\"}");
					send_ws_text("{\"acn\":\"asl\"}");
					if (!tried_http_seed) {
						tried_http_seed = true;
						seed_text_from_http_status("Initial");
					}
				}
			}
			if (acn == "psl") {
				std::string uid = extract_current_layout_uid(message);
				if (!uid.empty() && uid != current_layout_uid) {
					current_layout_uid = uid;
					requested.clear();
					log("Current Stage Display layout is " + current_layout_uid + ".");
					send_ws_text("{\"acn\":\"asl\"}");
				}
			}
			bool can_request_frame_values = !(acn == "asl" && current_layout_uid.empty());
			if (!can_request_frame_values)
				log("Received stage layouts before current layout; waiting for current layout before requesting frame values.");
			if (can_request_frame_values) {
				for (const std::string &uid : extract_frame_uids(message, settings_.channel, current_layout_uid)) {
					if (requested.insert(uid).second) {
						send_ws_text("{\"acn\":\"fv\",\"uid\":\"" + escape_json(uid) + "\"}");
						log("Requested Stage Display frame value " + uid + ".");
					}
				}
			}
			std::string text = extract_stage_text(message, settings_.channel);
			bool has_text = !text.empty() || message.find("\"acn\":\"" + settings_.channel + "\"") != std::string::npos;
			if (!has_text)
				has_text = extract_frame_value_text(message, settings_.channel, text);
			if (has_text)
				callback_(text);
			else if (message.find("\"acn\":\"fv\"") != std::string::npos)
				log("Received Stage Display frame value, but no text field matched the selected display channel.");
		}
		log("Stage Display WebSocket disconnected.");
		close_socket(socket_);
		socket_ = invalid_socket_handle;
	}

	void run()
	{
		socket_startup();
		while (!stopped_) {
			if (!settings_.service_id.empty()) {
				stage_discovery::Service service;
				if (!stage_discovery::parse_id(settings_.service_id, service) ||
				    !stage_discovery::resolve(service, stopped_)) {
					log("Waiting for the selected Stage Display instance to appear.");
					for (int i = 0; i < 30 && !stopped_; ++i)
						pause(std::chrono::milliseconds(100));
					continue;
				}
				settings_.host = service.host;
				settings_.port = service.port;
				log("Resolved " + service.name + " at " + service.host + ":" + std::to_string(service.port));
			}
			if (stopped_) break;
			if (settings_.api_mode == "http_status_poll") {
				run_http_poll_for(settings_.service_id.empty() ? std::chrono::seconds(86400) : std::chrono::seconds(5));
			} else {
				run_stage_session();
				if (settings_.api_mode == "stage_with_http_fallback")
					run_http_poll_for(std::chrono::seconds(4));
			}
			pause(std::chrono::milliseconds(1000));
		}
	}

	std::mutex control_mutex_;
	std::condition_variable control_changed_;
	bool pending_ = false;
	bool quitting_ = false;
	Settings pending_settings_;
	TextCallback pending_callback_;
	LogCallback pending_log_callback_;
	Settings settings_;
	TextCallback callback_;
	LogCallback log_callback_;
	std::atomic<bool> stopped_ = true;
	socket_handle socket_ = invalid_socket_handle;
	std::thread thread_;
};

static std::string presets_file_path()
{
	return module_config_file_path("presets.ini");
}

static std::map<std::string, Style> load_presets()
{
	std::map<std::string, Style> presets;
	presets["Default"] = Style();
	std::ifstream in(presets_file_path());
	std::string line;
	std::string current;
	while (std::getline(in, line)) {
		line = trim(line);
		if (line.empty())
			continue;
		if (line.front() == '[' && line.back() == ']') {
			current = line.substr(1, line.size() - 2);
			presets[current] = Style();
			continue;
		}
		if (current.empty())
			continue;

		size_t equals = line.find('=');
		if (equals == std::string::npos)
			continue;

		std::string key = trim(line.substr(0, equals));
		std::string value = trim(line.substr(equals + 1));
		Style &style = presets[current];
		auto as_int = [&] { return std::atoi(value.c_str()); };
		auto as_double = [&] { return std::atof(value.c_str()); };
		auto as_bool = [&] { return value == "1" || value == "true"; };
		auto as_color = [&] { return static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10)); };

		if (key == "text_color")
			style.text_color = as_color();
		else if (key == "text_opacity")
			style.text_opacity = as_int();
		else if (key == "font_family")
			style.font_family = value;
		else if (key == "font_size")
			style.font_size = as_int();
		else if (key == "font_weight")
			style.font_weight = value;
		else if (key == "font_style")
			style.font_style = value;
		else if (key == "letter_spacing")
			style.letter_spacing = as_double();
		else if (key == "line_height")
			style.line_height = as_double();
		else if (key == "text_align")
			style.text_align = value;
		else if (key == "vertical_align")
			style.vertical_align = value;
		else if (key == "text_transform")
			style.text_transform = value;
		else if (key == "max_lines")
			style.max_lines = as_int();
		else if (key == "disable_line_wrapping")
			style.disable_line_wrapping = as_bool();
		else if (key == "outer_padding_x")
			style.outer_padding_x = as_int();
		else if (key == "outer_padding_y")
			style.outer_padding_y = as_int();
		else if (key == "scaling")
			style.scaling = value == "shrink_to_fit" ? "shrink_to_fit" : "none";
		else if (key == "custom_css")
			style.custom_css = json_unescape(value);
		else if (key == "shadow_enabled")
			style.shadow_enabled = as_bool();
		else if (key == "shadow_color")
			style.shadow_color = as_color();
		else if (key == "shadow_opacity")
			style.shadow_opacity = as_int();
		else if (key == "shadow_x")
			style.shadow_x = as_int();
		else if (key == "shadow_y")
			style.shadow_y = as_int();
		else if (key == "shadow_blur")
			style.shadow_blur = as_int();
		else if (key == "line_background_enabled")
			style.line_background_enabled = as_bool();
		else if (key == "line_background_hide_when_empty")
			style.line_background_hide_when_empty = as_bool();
		else if (key == "line_background_color")
			style.line_background_color = as_color();
		else if (key == "line_background_opacity")
			style.line_background_opacity = as_int();
		else if (key == "line_background_padding_x")
			style.line_background_padding_x = as_int();
		else if (key == "line_background_padding_y")
			style.line_background_padding_y = as_int();
		else if (key == "line_gap")
			style.line_gap = as_int();
		else if (key == "line_background_radius")
			style.line_background_radius = as_int();
		else if (key == "crossfade_enabled")
			style.crossfade_enabled = as_bool();
		else if (key == "crossfade_ms")
			style.crossfade_ms = as_int();
	}
	return presets;
}

static void write_style_ini(std::ostream &out, const Style &style)
{
	out << "text_color=" << style.text_color << "\n";
	out << "text_opacity=" << style.text_opacity << "\n";
	out << "font_family=" << style.font_family << "\n";
	out << "font_size=" << style.font_size << "\n";
	out << "font_weight=" << style.font_weight << "\n";
	out << "font_style=" << style.font_style << "\n";
	out << "letter_spacing=" << style.letter_spacing << "\n";
	out << "line_height=" << style.line_height << "\n";
	out << "text_align=" << style.text_align << "\n";
	out << "vertical_align=" << style.vertical_align << "\n";
	out << "text_transform=" << style.text_transform << "\n";
	out << "max_lines=" << style.max_lines << "\n";
	out << "disable_line_wrapping=" << (style.disable_line_wrapping ? 1 : 0) << "\n";
	out << "outer_padding_x=" << style.outer_padding_x << "\n";
	out << "outer_padding_y=" << style.outer_padding_y << "\n";
	out << "scaling=" << style.scaling << "\n";
	out << "custom_css=" << escape_json(style.custom_css) << "\n";
	out << "shadow_enabled=" << (style.shadow_enabled ? 1 : 0) << "\n";
	out << "shadow_color=" << style.shadow_color << "\n";
	out << "shadow_opacity=" << style.shadow_opacity << "\n";
	out << "shadow_x=" << style.shadow_x << "\n";
	out << "shadow_y=" << style.shadow_y << "\n";
	out << "shadow_blur=" << style.shadow_blur << "\n";
	out << "line_background_enabled=" << (style.line_background_enabled ? 1 : 0) << "\n";
	out << "line_background_hide_when_empty=" << (style.line_background_hide_when_empty ? 1 : 0) << "\n";
	out << "line_background_color=" << style.line_background_color << "\n";
	out << "line_background_opacity=" << style.line_background_opacity << "\n";
	out << "line_background_padding_x=" << style.line_background_padding_x << "\n";
	out << "line_background_padding_y=" << style.line_background_padding_y << "\n";
	out << "line_gap=" << style.line_gap << "\n";
	out << "line_background_radius=" << style.line_background_radius << "\n";
	out << "crossfade_enabled=" << (style.crossfade_enabled ? 1 : 0) << "\n";
	out << "crossfade_ms=" << style.crossfade_ms << "\n";
}

static void save_presets(const std::map<std::string, Style> &presets)
{
	std::string path = presets_file_path();
	try {
		std::filesystem::path parent = std::filesystem::path(path).parent_path();
		if (!parent.empty())
			std::filesystem::create_directories(parent);
	} catch (...) {
	}

	std::ofstream out(path, std::ios::trunc);
	for (const auto &entry : presets) {
		if (entry.first == "Default")
			continue;
		out << "[" << entry.first << "]\n";
		write_style_ini(out, entry.second);
		out << "\n";
	}
}

struct ProPresenterLyricsSource {
	obs_source_t *source = nullptr;
	obs_source_t *browser = nullptr;
	bool browser_showing = false;
	Settings settings;
	OverlayState state;
	LogStore logs;
	OverlayServer server;
	StageDisplayClient client;
	std::mutex discovery_mutex;
	std::vector<stage_discovery::Service> discovered;
	std::atomic<bool> discovery_cancel{false};
	std::atomic<bool> scanning{false};
	std::thread discovery_thread;
	std::thread log_updates_thread;
	std::mutex log_updates_mutex;
	std::condition_variable log_updates_changed;
	bool log_updates_stopped = false;

	explicit ProPresenterLyricsSource(obs_source_t *source_) : source(source_), server(state) {}
};

// No network or render thread waits for a properties refresh. Changes are
// coalesced, and no refresh is emitted while the in-memory log is unchanged.
static void start_log_updates(ProPresenterLyricsSource *ctx)
{
	ctx->log_updates_thread = std::thread([ctx] {
		uint64_t published = 0;
		std::unique_lock<std::mutex> lock(ctx->log_updates_mutex);
		while (!ctx->log_updates_changed.wait_for(lock, std::chrono::milliseconds(500),
			[ctx] { return ctx->log_updates_stopped; })) {
			uint64_t revision = ctx->logs.revision();
			if (revision == published) continue;
			published = revision;
			lock.unlock();
			obs_source_update_properties(ctx->source);
			lock.lock();
		}
	});
}

static void stop_log_updates(ProPresenterLyricsSource *ctx)
{
	{
		std::lock_guard<std::mutex> lock(ctx->log_updates_mutex);
		ctx->log_updates_stopped = true;
	}
	ctx->log_updates_changed.notify_all();
	if (ctx->log_updates_thread.joinable()) ctx->log_updates_thread.join();
}

static std::string log_display_html(const std::string &text)
{
	// OBS info labels interpret rich text. Escape messages, including lyrics,
	// so text from ProPresenter cannot become markup or clickable links.
	std::string html = "<span style=\"font-family:monospace\">";
	for (char ch : text) {
		switch (ch) {
		case '&': html += "&amp;"; break;
		case '<': html += "&lt;"; break;
		case '>': html += "&gt;"; break;
		case '\n': html += "<br/>"; break;
		default: html += ch; break;
		}
	}
	return html + "</span>";
}

static void start_discovery(ProPresenterLyricsSource *ctx)
{
	if (ctx->scanning.exchange(true)) return;
	if (ctx->discovery_thread.joinable()) ctx->discovery_thread.join();
	ctx->discovery_thread = std::thread([ctx] {
		socket_startup();
		auto results = stage_discovery::scan(ctx->discovery_cancel);
		{
			std::lock_guard<std::mutex> lock(ctx->discovery_mutex);
			ctx->discovered = std::move(results);
		}
		ctx->scanning = false;
		if (!ctx->discovery_cancel) obs_source_update_properties(ctx->source);
	});
}

static void read_settings(obs_data_t *data, Settings &settings)
{
	settings.host = obs_data_get_string(data, "prop_host");
	settings.service_id = obs_data_get_string(data, "prop_service");
	settings.port = static_cast<int>(obs_data_get_int(data, "prop_port"));
	settings.password = obs_data_get_string(data, "prop_password");
	settings.api_mode = obs_data_get_string(data, "api_mode");
	settings.channel = obs_data_get_string(data, "display_channel");
	settings.poll_interval_ms = static_cast<int>(obs_data_get_int(data, "poll_interval_ms"));
	settings.overlay_port = static_cast<int>(obs_data_get_int(data, "overlay_port"));
	settings.width = static_cast<uint32_t>(obs_data_get_int(data, "width"));
	settings.height = static_cast<uint32_t>(obs_data_get_int(data, "height"));
	settings.preset_name = obs_data_get_string(data, "preset_name");
	settings.preset_to_apply = obs_data_get_string(data, "preset_to_apply");

	Style &s = settings.style;
	s.text_color = static_cast<uint32_t>(obs_data_get_int(data, "text_color"));
	s.text_opacity = static_cast<int>(obs_data_get_int(data, "text_opacity"));
	s.font_family = obs_data_get_string(data, "font_family");
	s.font_size = static_cast<int>(obs_data_get_int(data, "font_size"));
	s.font_weight = obs_data_get_string(data, "font_weight");
	s.font_style = obs_data_get_string(data, "font_style");
	s.letter_spacing = obs_data_get_double(data, "letter_spacing");
	s.line_height = obs_data_get_double(data, "line_height");
	s.text_align = obs_data_get_string(data, "text_align");
	s.vertical_align = obs_data_get_string(data, "vertical_align");
	s.text_transform = obs_data_get_string(data, "text_transform");
	s.max_lines = static_cast<int>(obs_data_get_int(data, "max_lines"));
	s.disable_line_wrapping = obs_data_get_bool(data, "disable_line_wrapping");
	s.outer_padding_x = static_cast<int>(obs_data_get_int(data, "outer_padding_x"));
	s.outer_padding_y = static_cast<int>(obs_data_get_int(data, "outer_padding_y"));
	s.scaling = obs_data_get_string(data, "scaling");
	if (s.scaling != "shrink_to_fit")
		s.scaling = "none";
	s.custom_css = obs_data_get_string(data, "custom_css");
	s.shadow_enabled = obs_data_get_bool(data, "shadow_enabled");
	s.shadow_color = static_cast<uint32_t>(obs_data_get_int(data, "shadow_color"));
	s.shadow_opacity = static_cast<int>(obs_data_get_int(data, "shadow_opacity"));
	s.shadow_x = static_cast<int>(obs_data_get_int(data, "shadow_x"));
	s.shadow_y = static_cast<int>(obs_data_get_int(data, "shadow_y"));
	s.shadow_blur = static_cast<int>(obs_data_get_int(data, "shadow_blur"));
	s.line_background_enabled = obs_data_get_bool(data, "line_background_enabled");
	s.line_background_hide_when_empty = obs_data_get_bool(data, "line_background_hide_when_empty");
	s.line_background_color = static_cast<uint32_t>(obs_data_get_int(data, "line_background_color"));
	s.line_background_opacity = static_cast<int>(obs_data_get_int(data, "line_background_opacity"));
	s.line_background_padding_x = static_cast<int>(obs_data_get_int(data, "line_background_padding_x"));
	s.line_background_padding_y = static_cast<int>(obs_data_get_int(data, "line_background_padding_y"));
	s.line_gap = static_cast<int>(obs_data_get_int(data, "line_gap"));
	s.line_background_radius = static_cast<int>(obs_data_get_int(data, "line_background_radius"));
	s.crossfade_enabled = obs_data_get_bool(data, "crossfade_enabled");
	s.crossfade_ms = static_cast<int>(obs_data_get_int(data, "crossfade_ms"));
}

static void update_browser(ProPresenterLyricsSource *ctx)
{
	obs_data_t *browser_settings = obs_data_create();
	obs_data_set_string(browser_settings, "url", ctx->server.url().c_str());
	obs_data_set_int(browser_settings, "width", ctx->settings.width);
	obs_data_set_int(browser_settings, "height", ctx->settings.height);
	obs_data_set_bool(browser_settings, "shutdown", false);
	obs_data_set_bool(browser_settings, "restart_when_active", false);

	if (!ctx->browser) {
		ctx->browser = obs_source_create_private("browser_source", "ProPresenter Lyrics Overlay", browser_settings);
		if (ctx->browser) {
			obs_source_inc_showing(ctx->browser);
			obs_source_inc_active(ctx->browser);
			ctx->browser_showing = true;
			ctx->logs.add("Internal Browser Source created at " + ctx->server.url());
		} else {
			ctx->logs.add("ERROR: Could not create OBS Browser Source. Make sure obs-browser is installed/enabled.");
		}
	} else {
		obs_source_update(ctx->browser, browser_settings);
		ctx->logs.add("Internal Browser Source updated at " + ctx->server.url());
	}

	obs_data_release(browser_settings);
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new ProPresenterLyricsSource(source);
	read_settings(settings, ctx->settings);
	ctx->state.setStyle(ctx->settings.style);
	ctx->logs.add("Source created. Overlay server starting on 127.0.0.1:" + std::to_string(ctx->settings.overlay_port));
	ctx->server.start(ctx->settings.overlay_port);
	update_browser(ctx);
	ctx->client.start(ctx->settings,
			  [ctx](const std::string &text) {
				  if (ctx->state.setText(text))
					  ctx->logs.add("Slide text updated: " + preview_text(text));
			  },
			  [ctx](const std::string &message) { ctx->logs.add(message); });
	start_discovery(ctx);
	start_log_updates(ctx);
	return ctx;
}

static void source_destroy(void *data)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	if (!ctx)
		return;
	stop_log_updates(ctx);
	ctx->discovery_cancel = true;
	if (ctx->discovery_thread.joinable()) ctx->discovery_thread.join();
	ctx->client.stop();
	ctx->server.stop();
	if (ctx->browser) {
		if (ctx->browser_showing) {
			obs_source_dec_active(ctx->browser);
			obs_source_dec_showing(ctx->browser);
		}
		obs_source_release(ctx->browser);
	}
	delete ctx;
}

static void source_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	if (!ctx)
		return;
	Settings previous = ctx->settings;
	read_settings(settings, ctx->settings);
	ctx->state.setStyle(ctx->settings.style);
	if (previous.overlay_port != ctx->settings.overlay_port || previous.width != ctx->settings.width ||
	    previous.height != ctx->settings.height) {
		ctx->logs.add("Overlay settings changed. Restarting overlay on 127.0.0.1:" +
			      std::to_string(ctx->settings.overlay_port));
		ctx->server.start(ctx->settings.overlay_port);
		update_browser(ctx);
	}
	if (previous.service_id != ctx->settings.service_id || previous.host != ctx->settings.host || previous.port != ctx->settings.port ||
	    previous.password != ctx->settings.password || previous.api_mode != ctx->settings.api_mode ||
	    previous.channel != ctx->settings.channel || previous.poll_interval_ms != ctx->settings.poll_interval_ms) {
		ctx->logs.add("Connection settings changed. Reconnecting to ProPresenter.");
		ctx->client.start(ctx->settings,
				  [ctx](const std::string &text) {
					  if (ctx->state.setText(text))
						  ctx->logs.add("Slide text updated: " + preview_text(text));
				  },
				  [ctx](const std::string &message) { ctx->logs.add(message); });
	}
}

static uint32_t source_width(void *data)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	return ctx ? ctx->settings.width : 1920;
}

static uint32_t source_height(void *data)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	return ctx ? ctx->settings.height : 1080;
}

static void source_render(void *data, gs_effect_t *)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	if (ctx && ctx->browser)
		obs_source_video_render(ctx->browser);
}

static void add_string_list(obs_properties_t *props, const char *name, const char *label,
			    const std::vector<std::pair<const char *, const char *>> &items)
{
	obs_property_t *property = obs_properties_add_list(props, name, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const auto &item : items)
		obs_property_list_add_string(property, item.first, item.second);
}

static bool open_log_file(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	if (!ctx)
		return false;

	std::string path = ctx->logs.path();
#ifdef _WIN32
	std::string command = "start \"\" \"" + path + "\"";
#elif __APPLE__
	std::string command = "/usr/bin/open -t " + shell_quote(path);
#else
	std::string command = "xdg-open " + shell_quote(path);
#endif
	std::system(command.c_str());
	return true;
}

static bool open_log_folder(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	if (!ctx)
		return false;

	std::filesystem::path folder = std::filesystem::path(ctx->logs.path()).parent_path();
	std::string path = folder.empty() ? "." : folder.string();
#ifdef _WIN32
	std::string command = "explorer \"" + path + "\"";
#elif __APPLE__
	std::string command = "/usr/bin/open " + shell_quote(path);
#else
	std::string command = "xdg-open " + shell_quote(path);
#endif
	std::system(command.c_str());
	return true;
}

static bool open_overlay_browser(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	if (!ctx)
		return false;

	std::string url = ctx->server.url();
#ifdef _WIN32
	std::string command = "start \"\" \"" + url + "\"";
#elif __APPLE__
	std::string command = "/usr/bin/open " + shell_quote(url);
#else
	std::string command = "xdg-open " + shell_quote(url);
#endif
	std::system(command.c_str());
	return true;
}

static bool save_preset(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	if (!ctx)
		return false;
	auto presets = load_presets();
	std::string name = trim(ctx->settings.preset_name.empty() ? "Preset" : ctx->settings.preset_name);
	presets[name] = ctx->settings.style;
	save_presets(presets);
	return true;
}

static bool apply_preset(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	if (!ctx)
		return false;
	auto presets = load_presets();
	auto found = presets.find(ctx->settings.preset_to_apply);
	if (found == presets.end())
		return false;
	ctx->settings.style = found->second;
	ctx->state.setStyle(ctx->settings.style);
	return true;
}

static bool delete_preset(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	if (!ctx || ctx->settings.preset_to_apply == "Default")
		return false;
	auto presets = load_presets();
	presets.erase(ctx->settings.preset_to_apply);
	save_presets(presets);
	return true;
}

static bool scan_displays(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	if (ctx) start_discovery(ctx);
	return true;
}

static bool display_selection_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool manual = std::string(obs_data_get_string(settings, "prop_service")).empty();
	obs_property_set_enabled(obs_properties_get(props, "prop_host"), manual);
	obs_property_set_enabled(obs_properties_get(props, "prop_port"), manual);
	return true;
}

static obs_properties_t *source_properties(void *data)
{
	auto *ctx = static_cast<ProPresenterLyricsSource *>(data);
	obs_properties_t *props = obs_properties_create();

	// Consecutive sibling tab groups form one native tab bar. OBS owns the
	// selection state and preserves it by group name across property refreshes.
	obs_properties_t *connection = obs_properties_create();
	auto *displays = obs_properties_add_list(connection, "prop_service", obs_module_text("StageDisplayInstance"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(displays, obs_module_text("ManualConnection"), "");
	if (ctx) {
		std::lock_guard<std::mutex> lock(ctx->discovery_mutex);
		bool selected_found = ctx->settings.service_id.empty();
		for (const auto &service : ctx->discovered) {
			std::string label = service.name + " (" + service.host + ":" + std::to_string(service.port) + ")";
			obs_property_list_add_string(displays, label.c_str(), service.id().c_str());
			if (service.id() == ctx->settings.service_id) selected_found = true;
		}
		if (!selected_found) {
			stage_discovery::Service saved;
			stage_discovery::parse_id(ctx->settings.service_id, saved);
			std::string label = saved.name + " — " + obs_module_text("DisplayUnavailable");
			obs_property_list_add_string(displays, label.c_str(), ctx->settings.service_id.c_str());
		}
	}
	obs_property_set_modified_callback(displays, display_selection_changed);
	auto *scan = obs_properties_add_button(connection, "scan_displays",
		obs_module_text(ctx && ctx->scanning ? "ScanningDisplays" : "ScanDisplays"), scan_displays);
	obs_property_set_enabled(scan, ctx && !ctx->scanning && stage_discovery::available());
	obs_properties_add_text(connection, "discovery_help",
		obs_module_text(stage_discovery::available() ? "DiscoveryHelp" : "DiscoveryUnavailable"), OBS_TEXT_INFO);
	obs_properties_add_text(connection, "prop_host", obs_module_text("ProPresenterIPAddress"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(connection, "prop_port", obs_module_text("ProPresenterPort"), 1, 65535, 1);
	obs_properties_add_text(connection, "prop_password", obs_module_text("ProPresenterPassword"), OBS_TEXT_PASSWORD);
	add_string_list(connection, "api_mode", obs_module_text("SyncMethod"),
			{{obs_module_text("StageDisplayWithFallback"), "stage_with_http_fallback"},
			 {obs_module_text("StageDisplayOnly"), "stage_display_ws"},
			 {obs_module_text("HTTPOnly"), "http_status_poll"}});
	add_string_list(connection, "display_channel", obs_module_text("DisplayText"),
			{{obs_module_text("CurrentSlideText"), "cs"},
			 {obs_module_text("NextSlideText"), "ns"},
			 {obs_module_text("CurrentSlideNotes"), "csn"},
			 {obs_module_text("NextSlideNotes"), "nsn"},
			 {obs_module_text("StageMessage"), "msg"}});
	obs_properties_add_int(connection, "poll_interval_ms", obs_module_text("HTTPPollInterval"), 150, 5000, 50);
	obs_properties_add_int(connection, "overlay_port", obs_module_text("OverlayPort"), 1024, 65535, 1);
	obs_properties_t *log = obs_properties_create();
	std::string log_text = ctx ? ctx->logs.text() : "Create the source to see live connection messages.";
	std::string log_html = log_display_html(log_text);
	auto *live_log = obs_properties_add_text(log, "live_connection_log", log_html.c_str(), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(live_log, true);
	obs_properties_add_button(log, "open_log_file", obs_module_text("OpenLogFile"), open_log_file);
	obs_properties_add_button(log, "open_log_folder", obs_module_text("OpenLogFolder"), open_log_folder);
	obs_properties_add_group(connection, "log", obs_module_text("LiveConnectionLog"), OBS_GROUP_NORMAL, log);
	obs_properties_add_group(props, "connection", obs_module_text("Connection"), OBS_GROUP_TAB, connection);

	obs_properties_t *size = obs_properties_create();
	obs_properties_add_int(size, "width", obs_module_text("Width"), 160, 7680, 10);
	obs_properties_add_int(size, "height", obs_module_text("Height"), 90, 4320, 10);
	obs_properties_add_group(props, "size", obs_module_text("Size"), OBS_GROUP_TAB, size);

	obs_properties_t *presets = obs_properties_create();
	obs_properties_add_text(presets, "preset_name", obs_module_text("PresetName"), OBS_TEXT_DEFAULT);
	obs_property_t *preset_list =
		obs_properties_add_list(presets, "preset_to_apply", obs_module_text("SavedPreset"), OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	for (const auto &entry : load_presets())
		obs_property_list_add_string(preset_list, entry.first.c_str(), entry.first.c_str());
	obs_properties_add_button(presets, "save_preset", obs_module_text("SavePreset"), save_preset);
	obs_properties_add_button(presets, "apply_preset", obs_module_text("ApplyPreset"), apply_preset);
	obs_properties_add_button(presets, "delete_preset", obs_module_text("DeletePreset"), delete_preset);
	obs_properties_add_group(props, "presets", obs_module_text("Presets"), OBS_GROUP_TAB, presets);

	obs_properties_t *typography = obs_properties_create();
	obs_properties_add_color(typography, "text_color", obs_module_text("TextColor"));
	obs_properties_add_int_slider(typography, "text_opacity", obs_module_text("TextOpacity"), 0, 100, 1);
	obs_properties_add_text(typography, "font_family", obs_module_text("FontFamily"), OBS_TEXT_DEFAULT);
	obs_properties_add_int_slider(typography, "font_size", obs_module_text("FontSize"), 8, 400, 1);
	add_string_list(typography, "font_weight", obs_module_text("FontWeight"),
			{{"Thin 100", "100"}, {"Extra light 200", "200"}, {"Light 300", "300"}, {"Regular 400", "400"},
			 {"Medium 500", "500"}, {"Semi-bold 600", "600"}, {"Bold 700", "700"},
			 {"Extra-bold 800", "800"}, {"Black 900", "900"}});
	add_string_list(typography, "font_style", obs_module_text("FontStyle"), {{"Normal", "normal"}, {"Italic", "italic"}});
	add_string_list(typography, "text_transform", obs_module_text("TextTransform"),
			{{"None", "none"}, {"Uppercase", "uppercase"}, {"Lowercase", "lowercase"}, {"Capitalize", "capitalize"}});
	obs_properties_add_float_slider(typography, "letter_spacing", obs_module_text("LetterSpacing"), -20.0, 80.0, 0.25);
	obs_properties_add_float_slider(typography, "line_height", obs_module_text("LineHeight"), 0.75, 3.0, 0.01);
	obs_properties_add_int_slider(typography, "max_lines", obs_module_text("MaxLines"), 0, 20, 1);
	obs_properties_add_bool(typography, "disable_line_wrapping", obs_module_text("DisableLineWrapping"));
	add_string_list(typography, "text_align", obs_module_text("TextAlignment"),
			{{"Left", "left"}, {"Center", "center"}, {"Right", "right"}, {"Justify", "justify"}});
	add_string_list(typography, "vertical_align", obs_module_text("VerticalAlignment"),
			{{"Top", "top"}, {"Middle", "middle"}, {"Bottom", "bottom"}});
	add_string_list(typography, "scaling", obs_module_text("Scaling"),
			{{"Shrink to fit", "shrink_to_fit"}, {"None", "none"}});
	obs_properties_add_int_slider(typography, "outer_padding_x", obs_module_text("HorizontalPadding"), 0, 1000, 1);
	obs_properties_add_int_slider(typography, "outer_padding_y", obs_module_text("VerticalPadding"), 0, 1000, 1);
	obs_properties_add_group(props, "typography", obs_module_text("Typography"), OBS_GROUP_TAB, typography);

	obs_properties_t *shadow = obs_properties_create();
	obs_properties_add_bool(shadow, "shadow_enabled", obs_module_text("TextShadow"));
	obs_properties_add_color(shadow, "shadow_color", obs_module_text("ShadowColor"));
	obs_properties_add_int_slider(shadow, "shadow_opacity", obs_module_text("ShadowOpacity"), 0, 100, 1);
	obs_properties_add_int_slider(shadow, "shadow_x", obs_module_text("ShadowX"), -100, 100, 1);
	obs_properties_add_int_slider(shadow, "shadow_y", obs_module_text("ShadowY"), -100, 100, 1);
	obs_properties_add_int_slider(shadow, "shadow_blur", obs_module_text("ShadowBlur"), 0, 200, 1);
	obs_properties_add_group(props, "shadow", obs_module_text("Shadow"), OBS_GROUP_TAB, shadow);

	obs_properties_t *line_background = obs_properties_create();
	obs_properties_add_bool(line_background, "line_background_enabled", obs_module_text("LineBackground"));
	obs_properties_add_bool(line_background, "line_background_hide_when_empty", obs_module_text("HideWhenEmpty"));
	obs_property_t *line_gap = obs_properties_add_int_slider(line_background, "line_gap", obs_module_text("LineGap"), 0, 1000, 1);
	obs_property_int_set_suffix(line_gap, " px");
	obs_properties_add_color(line_background, "line_background_color", obs_module_text("LineBackgroundColor"));
	obs_properties_add_int_slider(line_background, "line_background_opacity", obs_module_text("LineBackgroundOpacity"), 0, 100, 1);
	obs_properties_add_int_slider(line_background, "line_background_padding_x", obs_module_text("LineBackgroundPaddingX"), 0, 200, 1);
	obs_properties_add_int_slider(line_background, "line_background_padding_y", obs_module_text("LineBackgroundPaddingY"), 0, 100, 1);
	obs_properties_add_int_slider(line_background, "line_background_radius", obs_module_text("LineBackgroundRadius"), 0, 80, 1);
	obs_properties_add_group(props, "line_background", obs_module_text("LineBackground"), OBS_GROUP_TAB, line_background);

	obs_properties_t *transitions = obs_properties_create();
	obs_properties_add_bool(transitions, "crossfade_enabled", obs_module_text("Crossfade"));
	obs_properties_add_int_slider(transitions, "crossfade_ms", obs_module_text("CrossfadeDuration"), 0, 5000, 25);
	obs_properties_add_group(props, "transitions", obs_module_text("Transitions"), OBS_GROUP_TAB, transitions);

	obs_properties_t *custom_css = obs_properties_create();
	obs_property_t *css_help =
		obs_properties_add_text(custom_css, "custom_css_help", obs_module_text("CustomCSSHelp"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(css_help, true);
	obs_property_t *css_text =
		obs_properties_add_text(custom_css, "custom_css", obs_module_text("CustomCSS"), OBS_TEXT_MULTILINE);
	obs_property_text_set_monospace(css_text, true);
	obs_properties_add_button(custom_css, "open_overlay_browser", obs_module_text("OpenOverlayBrowser"), open_overlay_browser);
	obs_properties_add_group(props, "custom_css_group", obs_module_text("CustomCSSTab"), OBS_GROUP_TAB, custom_css);


	return props;
}

static void source_defaults(obs_data_t *settings)
{
	Style style;
	obs_data_set_default_string(settings, "prop_host", "127.0.0.1");
	obs_data_set_default_int(settings, "prop_port", 50001);
	obs_data_set_default_string(settings, "prop_password", "");
	obs_data_set_default_string(settings, "api_mode", "stage_with_http_fallback");
	obs_data_set_default_string(settings, "display_channel", "cs");
	obs_data_set_default_int(settings, "poll_interval_ms", 500);
	obs_data_set_default_int(settings, "overlay_port", 39110);
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_string(settings, "preset_name", "Default");
	obs_data_set_default_string(settings, "preset_to_apply", "Default");
	obs_data_set_default_int(settings, "text_color", style.text_color);
	obs_data_set_default_int(settings, "text_opacity", style.text_opacity);
	obs_data_set_default_string(settings, "font_family", style.font_family.c_str());
	obs_data_set_default_int(settings, "font_size", style.font_size);
	obs_data_set_default_string(settings, "font_weight", style.font_weight.c_str());
	obs_data_set_default_string(settings, "font_style", style.font_style.c_str());
	obs_data_set_default_double(settings, "letter_spacing", style.letter_spacing);
	obs_data_set_default_double(settings, "line_height", style.line_height);
	obs_data_set_default_string(settings, "text_align", style.text_align.c_str());
	obs_data_set_default_string(settings, "vertical_align", style.vertical_align.c_str());
	obs_data_set_default_string(settings, "text_transform", style.text_transform.c_str());
	obs_data_set_default_int(settings, "max_lines", style.max_lines);
	obs_data_set_default_bool(settings, "disable_line_wrapping", style.disable_line_wrapping);
	obs_data_set_default_int(settings, "outer_padding_x", style.outer_padding_x);
	obs_data_set_default_int(settings, "outer_padding_y", style.outer_padding_y);
	obs_data_set_default_string(settings, "scaling", style.scaling.c_str());
	obs_data_set_default_string(settings, "custom_css", style.custom_css.c_str());
	obs_data_set_default_bool(settings, "shadow_enabled", style.shadow_enabled);
	obs_data_set_default_int(settings, "shadow_color", style.shadow_color);
	obs_data_set_default_int(settings, "shadow_opacity", style.shadow_opacity);
	obs_data_set_default_int(settings, "shadow_x", style.shadow_x);
	obs_data_set_default_int(settings, "shadow_y", style.shadow_y);
	obs_data_set_default_int(settings, "shadow_blur", style.shadow_blur);
	obs_data_set_default_bool(settings, "line_background_enabled", style.line_background_enabled);
	obs_data_set_default_bool(settings, "line_background_hide_when_empty", style.line_background_hide_when_empty);
	obs_data_set_default_int(settings, "line_background_color", style.line_background_color);
	obs_data_set_default_int(settings, "line_background_opacity", style.line_background_opacity);
	obs_data_set_default_int(settings, "line_background_padding_x", style.line_background_padding_x);
	obs_data_set_default_int(settings, "line_background_padding_y", style.line_background_padding_y);
	obs_data_set_default_int(settings, "line_gap", style.line_gap);
	obs_data_set_default_int(settings, "line_background_radius", style.line_background_radius);
	obs_data_set_default_bool(settings, "crossfade_enabled", style.crossfade_enabled);
	obs_data_set_default_int(settings, "crossfade_ms", style.crossfade_ms);
}

static const char *source_name(void *)
{
	return obs_module_text("ProPresenterLyrics");
}

bool obs_module_load(void)
{
	obs_source_info info = {};
	info.id = plugin_id;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	info.get_name = source_name;
	info.create = source_create;
	info.destroy = source_destroy;
	info.update = source_update;
	info.get_defaults = source_defaults;
	info.get_properties = source_properties;
	info.get_width = source_width;
	info.get_height = source_height;
	info.video_render = source_render;
	obs_register_source(&info);
	blog(LOG_INFO, "[propresenter-lyrics] loaded version %s", PROJECT_VERSION);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[propresenter-lyrics] unloaded");
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Displays live ProPresenter Stage Display text as a native OBS source.";
}
