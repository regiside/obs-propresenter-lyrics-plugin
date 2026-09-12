#include "../src/stage-discovery.hpp"
#include <cassert>
#include <iostream>

int main(int argc, char **argv)
{
	using namespace stage_discovery;
	Service original{"Stage.Name \\ Test", "_pro7stagedsply._tcp.", "local.", "old.local.", 50001};
	Service restored;
	assert(parse_id(original.id(), restored));
	assert(restored.id() == original.id());
	assert(restored.host.empty() && restored.port == 0);
	assert(!parse_id("", restored));
	assert(!parse_id("host\t_http._tcp.\tlocal.", restored));
	assert(!parse_id("host\t_pro7stagedsply._tcp.\t", restored));
	std::atomic<bool> cancel{true};
	auto start = std::chrono::steady_clock::now();
	assert(scan(cancel).empty());
	assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
	if (argc > 1 && std::string(argv[1]) == "--live") {
		cancel = false;
		auto services = scan(cancel);
		assert(!services.empty());
		for (const auto &service : services) {
			assert(parse_id(service.id(), restored));
			// Reconstruct from only the persisted identity, then resolve afresh.
			assert(resolve(restored, cancel));
			assert(restored.host == service.host && restored.port == service.port);
			std::cout << service.name << " -> " << restored.host << ":" << restored.port << '\n';
		}
		Service missing{"codex-test-absent-instance-957126", "_pro7stagedsply._tcp.", "local.", "old.local.", 1234};
		assert(!resolve(missing, cancel));
		assert(missing.host.empty() && missing.port == 0);
	}
	std::cout << "Discovery tests passed\n";
}
