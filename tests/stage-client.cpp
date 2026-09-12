// Exercise the real connection worker without launching OBS.
#include "../src/propresenter-lyrics.cpp"
#include <cassert>
#include <iostream>

class SilentServer {
public:
	explicit SilentServer(bool upgrade) : upgrade_(upgrade)
	{
		listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		assert(listener_ != invalid_socket_handle);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		assert(bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
		assert(listen(listener_, 8) == 0);
#ifdef _WIN32
		int length = sizeof(address);
#else
		socklen_t length = sizeof(address);
#endif
		assert(getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length) == 0);
		port = ntohs(address.sin_port);
		thread_ = std::thread([this] {
			while (!done_) {
				fd_set reads;
				FD_ZERO(&reads);
				FD_SET(listener_, &reads);
				timeval timeout{0, 10000};
				if (select(static_cast<int>(listener_ + 1), &reads, nullptr, nullptr, &timeout) <= 0) continue;
				auto peer = accept(listener_, nullptr, nullptr);
				if (peer == invalid_socket_handle) continue;
				peers_.push_back(peer);
				if (upgrade_) {
					const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
					send(peer, response.data(), static_cast<int>(response.size()), 0);
				}
				++accepted;
			}
			for (auto peer : peers_) close_socket(peer);
		});
	}
	~SilentServer() { done_ = true; thread_.join(); close_socket(listener_); }
	int port;
	std::atomic<int> accepted{0};
private:
	bool upgrade_;
	std::atomic<bool> done_{false};
	socket_handle listener_;
	std::thread thread_;
	std::vector<socket_handle> peers_;
};

static void fast(const char *label, const std::function<void()> &action)
{
	auto start = std::chrono::steady_clock::now();
	action();
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	std::cout << label << ": " << elapsed << " ms\n";
	assert(elapsed < 300);
}

static void check_tab_compatibility()
{
	// Exercise lookup and the real connection callback through a tab group
	// using the baseline libobs, which predates the tab enum declaration.
	auto *props = obs_properties_create();
	auto *connection = obs_properties_create();
	auto *selection = obs_properties_add_list(connection, "prop_service", "Instance",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(selection, display_selection_changed);
	auto *host = obs_properties_add_text(connection, "prop_host", "Host", OBS_TEXT_DEFAULT);
	auto *port = obs_properties_add_int(connection, "prop_port", "Port", 1, 65535, 1);
	auto *tab = obs_properties_add_group(props, "connection", "Connection", OBS_GROUP_TAB, connection);
	assert(tab && obs_property_group_type(tab) == OBS_GROUP_TAB);
	assert(obs_properties_get(props, "prop_host") == host);
	auto *settings = obs_data_create();
	obs_data_set_string(settings, "prop_host", "127.0.0.1");
	obs_data_set_string(settings, "prop_service", "saved instance");
	obs_properties_apply_settings(props, settings);
	assert(!obs_property_enabled(host) && !obs_property_enabled(port));
	obs_data_set_string(settings, "prop_service", "");
	obs_properties_apply_settings(props, settings);
	assert(obs_property_enabled(host) && obs_property_enabled(port));
	assert(std::string(obs_data_get_string(settings, "prop_host")) == "127.0.0.1");
	assert(!obs_data_has_user_value(settings, "connection"));
	obs_data_release(settings);
	obs_properties_destroy(props);
	std::cout << "Tab compatibility and nested connection callback checks passed\n";
}

static void check_live_log()
{
	auto path = std::filesystem::temp_directory_path() /
		("propresenter-live-log-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
	{
		LogStore logs(path.string());
		assert(logs.revision() == 0);
		for (int i = 0; i < 20; ++i) logs.add("test-message-" + std::to_string(i) + "!");
		assert(logs.revision() == 20);
		std::ofstream(path) << "This disk-only text must not appear in the live view";
		std::string text = logs.text();
		assert(text.find("disk-only") == std::string::npos);
		assert(text.find("test-message-0!") == std::string::npos);
		assert(text.find("test-message-19!") != std::string::npos);
		assert(logs.revision() == 20); // reading never creates refresh events
		logs.add("<a href='test'>lyrics & notes</a>");
		std::string html = log_display_html(logs.text());
		assert(html.find("<a href=") == std::string::npos);
		assert(html.find("&lt;a href=") != std::string::npos);
		assert(html.find("lyrics &amp; notes") != std::string::npos);
	}
	std::filesystem::remove(path);
	std::cout << "Live log memory, revision, history, and escaping checks passed\n";
}

int main()
{
	check_live_log();
	check_tab_compatibility();
	socket_startup();
	StageDisplayClient client;
	auto text = [](const std::string &) {};
	auto log = [](const std::string &) {};
	Settings settings;
	settings.api_mode = "stage_only";
	for (bool upgrade : {false, true}) {
		SilentServer server(upgrade);
		settings.port = server.port;
		client.start(settings, text, log);
		for (int i = 0; i < 200 && !server.accepted; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		assert(server.accepted);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		fast(upgrade ? "idle WebSocket update" : "stalled handshake update", [&] {
			for (int i = 0; i < 100; ++i) client.start(settings, text, log);
		});
		fast("cancel socket and stop", [&] { client.stop(); });
	}
	{
		SilentServer server(false);
		settings.port = server.port;
		settings.api_mode = "http_status_poll";
		client.start(settings, text, log);
		for (int i = 0; i < 200 && !server.accepted; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		assert(server.accepted);
		fast("cancel stalled HTTP response", [&] { client.stop(); });
	}
	settings.host = "192.0.2.1";
	settings.port = 50001;
	client.start(settings, text, log);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	fast("cancel unavailable TCP host", [&] { client.stop(); });
	settings.host = "codex-absent-host-957126.local.";
	client.start(settings, text, log);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	fast("cancel hostname lookup", [&] { client.stop(); });
	settings.service_id = "codex-absent-instance-957126\t_pro7stagedsply._tcp.\tlocal.";
	client.start(settings, text, log);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	fast("cancel Bonjour service resolution", [&] { client.stop(); });
	{
		SilentServer server(true);
		std::atomic<bool> connected{false};
		client.start(settings, text, log); // an unavailable saved service
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		settings.service_id.clear();
		settings.host = "localhost";
		settings.port = server.port;
		settings.api_mode = "stage_only";
		fast("switch from missing service to available server", [&] {
			client.start(settings, text, [&](const std::string &message) {
				if (message == "Stage Display WebSocket connected.") connected = true;
			});
		});
		for (int i = 0; i < 400 && !connected; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		assert(connected);
		fast("stop recovered connection", [&] { client.stop(); });
	}
	std::cout << "Connection responsiveness checks passed\n";
}
