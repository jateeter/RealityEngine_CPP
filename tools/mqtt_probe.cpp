// mqtt_probe — connect to a broker, subscribe to #, print every PUBLISH
// for N seconds, exit.  Used to discover what topics + payload shapes
// live on a broker before authoring a mqtt-mappings.json.
//
// Usage:
//   mqtt_probe <host> [port] [duration-seconds] [topic-filter]
//
// Builds on the MqttClient from reality/mqtt_client.hpp — same code path
// the PerceptionService's MqttBridge uses, so what this prints is exactly
// what the bridge would see.

#include "reality/mqtt_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>

namespace mq = reality::mqtt;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <host> [port=1883] [seconds=20] [topicFilter=#]\n";
    return 2;
  }
  std::string host   = argv[1];
  int port           = (argc > 2) ? std::atoi(argv[2]) : 1883;
  int seconds        = (argc > 3) ? std::atoi(argv[3]) : 20;
  std::string filter = (argc > 4) ? argv[4] : "#";

  mq::ClientConfig cfg;
  cfg.brokerHost       = host;
  cfg.brokerPort       = port;
  cfg.clientId         = "re-mqtt-probe-" + std::to_string(::getpid());
  cfg.keepaliveSec     = 30;
  cfg.reconnectDelayMs = 5000;

  mq::MqttClient client(cfg);
  std::mutex mu;
  std::set<std::string> uniqueTopics;
  std::atomic<int> total{0};

  client.set_message_handler([&](const std::string& topic, const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(mu);
    ++total;
    bool newTopic = uniqueTopics.insert(topic).second;
    // Render the payload — UTF-8 if printable, otherwise a "<N bytes>" placeholder.
    std::string sample;
    bool printable = true;
    for (uint8_t b : payload) {
      if (b < 0x20 && b != '\n' && b != '\r' && b != '\t') { printable = false; break; }
      if (b > 0x7E) { printable = false; break; }
    }
    if (printable) sample.assign(payload.begin(), payload.end());
    else           sample = "<" + std::to_string(payload.size()) + " bytes binary>";
    if (sample.size() > 200) sample = sample.substr(0, 200) + "…";

    std::cout << (newTopic ? "[NEW] " : "      ") << topic << "  ⟶  " << sample << "\n";
  });

  client.subscribe(filter, 0);
  std::cout << "Connecting to " << host << ":" << port << " ... (filter=" << filter
            << ", duration=" << seconds << "s)\n";
  client.start();

  std::this_thread::sleep_for(std::chrono::seconds(seconds));

  std::cout << "\n──── summary ────\n";
  {
    std::lock_guard<std::mutex> lock(mu);
    std::cout << "total messages:  " << total.load() << "\n";
    std::cout << "unique topics:   " << uniqueTopics.size() << "\n";
    if (!uniqueTopics.empty()) {
      std::cout << "\ntopics seen:\n";
      for (const auto& t : uniqueTopics) std::cout << "  " << t << "\n";
    }
  }

  client.stop();
  return 0;
}
