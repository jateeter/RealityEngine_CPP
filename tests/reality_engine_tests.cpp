#include "reality/reality.hpp"

#include <cassert>
#include <iostream>

using namespace reality;

static Machine make_rs_like_machine() {
  PerceptualMapping mapping{{0, 2}, {10, 2}};
  Machine m("Test Machine", "smoke test", ArbiterRule::Passthrough, mapping, "machine-test");
  CriticalEventSequence seq("A then output", "seq-test");

  RealityVector start({VectorElement{1.0, ComparatorType::Gte, 0.5}, VectorElement{0.0, ComparatorType::Gte, 0.5}}, true, "start");
  start.add_next_vector("terminal");

  RealityVector terminal({VectorElement{0.0, ComparatorType::Gte, 0.5}, VectorElement{1.0, ComparatorType::Gte, 0.5}}, false, "terminal");
  terminal.add_output_vector({"out", {1.0, 0.0}, {{"description", "terminal fired"}}, now_ms()});

  seq.add_vector(start);
  seq.add_vector(terminal);
  m.add_sequence(seq);
  return m;
}

int main() {
  {
    RealityVector v({VectorElement{1.0, ComparatorType::Gte, 0.5}}, true, "v");
    assert(v.match({0.75}).matched);
    assert(!v.match({0.25}).matched);
  }

  {
    Machine m = make_rs_like_machine();
    auto r1 = m.process_input({1.0, 0.0});
    assert(r1.arbiterMetadata.shouldOutput == false);
    auto r2 = m.process_input({0.0, 1.0});
    assert(r2.arbiterMetadata.shouldOutput == true);
    assert(r2.machineOutput.has_value());
  }

  {
    Machine m = make_rs_like_machine();
    PerceptualSpaceSimulator sim(256);
    sim.add_machine(m);
    Vector ps(256, 0.0);
    ps[0] = 1.0;
    auto s1 = sim.process_immediate(ps);
    assert(s1.machineResults.count("machine-test") == 1);
    ps[0] = 0.0;
    ps[1] = 1.0;
    auto s2 = sim.process_immediate(ps);
    assert(s2.perceptualSpace[10] == 1.0);
  }

  {
    PerceptionEngine pe;
    SourceConfig src;
    src.kind = "test";
    src.name = "source";
    src.region = {4, 2};
    src.inputs = {{0.2, 0.8}};
    auto added = pe.add_source(src);
    auto assembled = pe.assemble_vector();
    assert(assembled[4] == 0.2);
    assert(assembled[5] == 0.8);
    pe.remove_source(added.id);
  }

  {
    PerceptionEngine pe;
    SourceConfig sensor;
    sensor.kind = "sensor";
    sensor.id = "stable-source-id";
    sensor.name = "localai/rag_retrieval";
    sensor.sensorId = "localai_rag_retrieval";
    sensor.region = {52, 4};
    sensor.ttlMs = 30000;
    auto added = pe.add_source(sensor);
    assert(added.id == "stable-source-id");
    assert(pe.update_sensor_value("localai_rag_retrieval", {0.4, 0.8, 0.0, 0.0}));
    auto assembled = pe.assemble_vector();
    assert(assembled[52] == 0.4);
    assert(assembled[53] == 0.8);
  }

  std::cout << "RealityEngine_CPP smoke tests passed\n";
  return 0;
}
