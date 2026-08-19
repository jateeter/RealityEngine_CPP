#include "reality/reality.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace reality;

static Machine make_rs_like_machine() {
  PerceptualMapping mapping{{0, 2}, {10, 2}};
  Machine m("Test Machine", "smoke test", ArbiterRule::Passthrough, mapping, "machine-test");
  CriticalEventSequence seq("A then output", "seq-test");

  RealityVector start({VectorElement{1.0, ComparatorType::Gte, 0.5}, VectorElement{0.0, ComparatorType::Gte, 0.5}}, true, "start");
  start.add_next_vector("terminal");

  RealityVector terminal({VectorElement{0.0, ComparatorType::Gte, 0.5}, VectorElement{1.0, ComparatorType::Gte, 0.5}}, false, "terminal");
  terminal.add_output_vector({"out", {1.0, 0.0}, {{"description", "terminal fired"}}, now_ms(), {}});

  seq.add_vector(start);
  seq.add_vector(terminal);
  m.add_sequence(seq);
  return m;
}

static Machine make_output_machine(const std::string& id, double outputValue) {
  PerceptualMapping mapping{{0, 1}, {20, 1}};
  Machine m("Output " + id, "deterministic merge test", ArbiterRule::Passthrough, mapping, id);
  CriticalEventSequence seq("Immediate output", "seq-" + id);

  RealityVector start({VectorElement{1.0, ComparatorType::Gte, 0.5}}, true, "start-" + id);
  start.add_output_vector({"out-" + id, {outputValue}, {{"description", "merge policy output"}}, now_ms(), {}});

  seq.add_vector(start);
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
    PerceptualSpaceSimulator sim(256);
    sim.add_machine(make_output_machine("machine-b", 0.8));
    sim.add_machine(make_output_machine("machine-a", 0.2));
    Vector ps(256, 0.0);
    ps[0] = 1.0;
    auto step = sim.process_immediate(ps);
    assert(step.machineResults.count("machine-a") == 1);
    assert(step.machineResults.count("machine-b") == 1);
    assert(step.perceptualSpace[20] == 0.8);
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

  {
    Vector cells{0.0, 1.0, 2.0, 3.0, 0.0, 1.0, 2.0, 3.0, 2.0};
    auto packed = pack_cells(cells, 2);
    assert(packed.size() == 3);
    assert(packed[0] == 0x1B);
    assert(unpack_cells(packed, cells.size(), 2) == cells);
    assert(encode_packed_base64({0.0, 1.0, 2.0, 3.0}, 2) == "Gw==");

    auto footprint = storage_footprint(4128, 2);
    assert(footprint.float64Bytes == 33024);
    assert(footprint.packedBytes == 1032);
    assert(footprint.shrinkFactor == 32.0);

    bool rejected = false;
    try {
      (void)pack_cells({4.0}, 2);
    } catch (const std::range_error&) {
      rejected = true;
    }
    assert(rejected);
  }

  {
    const std::string raw = R"({
      "version": "1.0.0",
      "machine": {
        "name": "Packed Mapping",
        "description": "bitsPerElement loader test",
        "perceptualMapping": {
          "input": {"offset": 0, "length": 2},
          "output": {"offset": 2, "length": 4},
          "bitsPerElement": 2
        },
        "sequences": []
      }
    })";
    Machine m = load_machine_from_json_string(raw, "machine-packed");
    assert(m.perceptualMapping.has_value());
    assert(m.perceptualMapping->bitsPerElement.has_value());
    assert(*m.perceptualMapping->bitsPerElement == 2);
    Json mapping = to_json(*m.perceptualMapping);
    assert(mapping.at("bitsPerElement").as_number() == 2.0);
  }

  {
    // Trajectory histories — SURFACE_SPEC.md, "Trajectory histories".
    //
    // ISRE(n) is what the corpus was presented with, OREV(n) what it produced.
    // A machine at input [0,1] output [20,1] firing 1.0 gives a step whose ISRE
    // carries the pushed input and whose OREV carries the output cell — which
    // is the whole shape of the claim, at one machine.
    PerceptualSpaceSimulator sim(32);
    sim.add_machine(make_output_machine("machine-traj", 1.0));

    Vector input(32, 0.0);
    input[0] = 1.0;
    sim.process_immediate(input);

    auto isre = sim.isre_history();
    auto orev = sim.orev_history();
    assert(isre.size() == 1);
    assert(orev.size() == 1);

    // ISRE is the space as presented: the pushed input, before the corpus ran.
    assert(isre[0].stepNumber == 0);
    assert(isre[0].length == 32);
    assert(isre[0].nonZero.size() == 1);
    assert(isre[0].nonZero[0].index == 0);
    assert(isre[0].nonZero[0].value == 1.0);

    // OREV is what the corpus produced: the output cell, not the input it read.
    assert(orev[0].stepNumber == 0);
    assert(orev[0].nonZero.size() == 1);
    assert(orev[0].nonZero[0].index == 20);
    assert(orev[0].nonZero[0].value == 1.0);

    // Ascending stepNumber — these are compared by index across engines, so a
    // newest-first history would report every step as the first divergence.
    sim.process_immediate(input);
    assert(sim.isre_history().size() == 2);
    assert(sim.isre_history()[0].stepNumber == 0);
    assert(sim.isre_history()[1].stepNumber == 1);

    // ISRE records what was presented, not what the engine remembered.
    // process_immediate replaces the space with the pushed vector, so the
    // second step's ISRE carries the input alone — the first step's output at
    // cell 20 is gone. The arbitration feedback that makes ISRE(n) differ from
    // ISRESeed(n) travels back through the Perception Engine, which reads the
    // output regions and assembles them into the next push. Asserting the
    // feedback here would be asserting a loop this object does not close.
    const TrajectoryEntry second = sim.isre_history()[1];
    assert(second.nonZero.size() == 1);
    assert(second.nonZero[0].index == 0);

    Json entry = to_json(orev[0]);
    assert(entry.at("stepNumber").as_number() == 0.0);
    assert(entry.at("length").as_number() == 32.0);
    const Json& cells = entry.at("nonZero");
    assert(cells.array().size() == 1);
    assert(cells.array()[0].at("index").as_number() == 20.0);

    sim.reset();
    assert(sim.isre_history().empty());
    assert(sim.orev_history().empty());

    // stepNumber restarts with the history. It used to keep counting while the
    // history was cleared, so a reset engine's first entry was stepNumber 2
    // here and 0 on LSP — and these histories are compared by stepNumber.
    sim.process_immediate(input);
    assert(sim.isre_history().size() == 1);
    assert(sim.isre_history()[0].stepNumber == 0);
    assert(sim.orev_history()[0].stepNumber == 0);
  }

  std::cout << "RealityEngine_CPP smoke tests passed\n";
  return 0;
}
