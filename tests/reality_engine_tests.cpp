#include "reality/reality.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>

using namespace reality;

// ── output merge fold ────────────────────────────────────────────────────────
//
// The specification these check against is
// RealityEngine_CI/scripts/experiment-mv-transforms.py, which verifies the same
// properties by exhaustion in Python. Restating them here rather than trusting
// the port is the point: the fold is what every runtime presents to the
// Perception Engine, and a C++ fold that agrees with the reference on the
// worked examples but not on the whole chain is exactly the class of drift that
// produced RealityEngine_CI#154 and #158.

// Fold a collection of scalar chain values, one cell wide — the shape the
// specification's property checks are written over.
static long long fold_scalar(const std::vector<long long>& values,
                             OutputMergeTransformation t,
                             std::optional<int> chainTop) {
  std::vector<Vector> outputs;
  outputs.reserve(values.size());
  for (long long v : values) outputs.push_back(Vector{static_cast<double>(v)});
  auto folded = fold_outputs(outputs, t, chainTop);
  assert(folded.has_value() && folded->size() == 1);
  return static_cast<long long>((*folded)[0]);
}

// Step `combo` as an odometer over the chain {0..k}. Returns false once it
// wraps, i.e. once every collection of that size has been visited.
static bool next_combination(std::vector<long long>& combo, long long k) {
  for (size_t i = 0; i < combo.size(); ++i) {
    if (++combo[i] <= k) return true;
    combo[i] = 0;
  }
  return false;
}

// Closure, symmetry, no-fabrication and idempotence, exhaustively over the
// chain {0..3} for collections up to n=4.
//
// Symmetry is the load-bearing one. The collection of potential outputs carries
// no order and the runtimes enumerate it differently, so an order-dependent
// fold would present a different vector per runtime for the same corpus.
static void verify_mv_transform_properties() {
  constexpr long long k = 3;
  const OutputMergeTransformation transforms[] = {
    OutputMergeTransformation::Meet,
    OutputMergeTransformation::Join,
    OutputMergeTransformation::StrongConjunction,
    OutputMergeTransformation::StrongDisjunction,
    OutputMergeTransformation::DiscreteMedian,
  };
  // Meet, join and median return one of their contributors and are idempotent.
  // The Łukasiewicz pair is neither, by design, and is not checked for either.
  const auto returns_a_contributor = [](OutputMergeTransformation t) {
    return t == OutputMergeTransformation::Meet ||
           t == OutputMergeTransformation::Join ||
           t == OutputMergeTransformation::DiscreteMedian;
  };

  for (const auto t : transforms) {
    for (size_t n = 1; n <= 4; ++n) {
      std::vector<long long> combo(n, 0);
      do {
        const long long got = fold_scalar(combo, t, static_cast<int>(k));

        // Closure — the fold may not leave the chain.
        assert(got >= 0 && got <= k);

        // Symmetry — invariant under every permutation of the collection.
        std::vector<long long> perm = combo;
        std::sort(perm.begin(), perm.end());
        do {
          assert(fold_scalar(perm, t, static_cast<int>(k)) == got);
        } while (std::next_permutation(perm.begin(), perm.end()));

        // No fabrication — stronger than closure. Bitwise or of 1 and 2 is 3,
        // inside the chain but asserted by nobody; on a severity ladder that
        // invents a rung.
        if (returns_a_contributor(t)) {
          assert(std::find(combo.begin(), combo.end(), got) != combo.end());
        }
      } while (next_combination(combo, k));

      if (returns_a_contributor(t)) {
        for (long long x = 0; x <= k; ++x) {
          assert(fold_scalar(std::vector<long long>(n, x), t, static_cast<int>(k)) == x);
        }
      }
    }
  }
}

// ⊙ is the De Morgan dual of ⊕ under ¬x = k − x, as the Łukasiewicz definition
// requires. Checked rather than assumed because the two are implemented
// independently here — ⊙ from its closed form, ⊕ from a saturating sum — and
// nothing else would catch one of them drifting.
static void verify_de_morgan_duality() {
  constexpr long long k = 3;
  for (size_t n = 1; n <= 4; ++n) {
    std::vector<long long> combo(n, 0);
    do {
      const long long direct =
        fold_scalar(combo, OutputMergeTransformation::StrongConjunction, static_cast<int>(k));
      std::vector<long long> negated;
      negated.reserve(n);
      for (long long x : combo) negated.push_back(k - x);
      const long long dual =
        k - fold_scalar(negated, OutputMergeTransformation::StrongDisjunction, static_cast<int>(k));
      assert(direct == dual);
    } while (next_combination(combo, k));
  }
}

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

// ── the fold in the machine's atomic step ────────────────────────────────────
//
// FOLD_PLACEMENT.md, "Implementation contract". The fold used to sit beside the
// machine's step rather than in it: its result was reported as
// mergedOutputVector and the arbiter still resolved the unfolded collection, so
// a machine with seven completed Reality Events contributed seven values to the
// same cell and the CELL arbiter settled contention that belonged to the
// MACHINE. FallDetection resolved to 2.0 on C++ and LSP and 0.0 on Scala that
// way — neither the maximum nor the minimum (RealityEngine_CI#154).
//
// The four properties below are the ones that can be got wrong silently.

// A machine whose sequences all match the same one-cell input and each assert
// their own output over the machine's output region. `outputs` is keyed by
// sequence id so a test can control which sequence sorts where — the tie-break
// and the severity join are only distinguishable when the winner is NOT the
// first sequence enumerated.
static Machine make_multi_sequence_machine(const std::string& id,
                                           const std::map<std::string, Vector>& outputs,
                                           int outputOffset,
                                           int outputLength) {
  PerceptualMapping mapping{{0, 1}, {outputOffset, outputLength}};
  Machine m("Multi " + id, "fold placement test", ArbiterRule::Passthrough, mapping, id);
  for (const auto& [sequenceId, values] : outputs) {
    CriticalEventSequence seq("seq " + sequenceId, sequenceId);
    RealityVector start({VectorElement{1.0, ComparatorType::Gte, 0.5}}, true, sequenceId + "-v1");
    start.add_output_vector({sequenceId + "-out", values, {}, now_ms(), {}});
    seq.add_vector(start);
    m.add_sequence(seq);
  }
  return m;
}

static Vector fire_at(int dimension) {
  Vector v(static_cast<size_t>(dimension), 0.0);
  v[0] = 1.0;
  return v;
}

// A machine that fired exactly one CES must reach arbitration exactly as it did
// before the fold moved: the same values, in one operation, under the same
// governance. That property is what leaves the overwhelming majority of the
// 1328-machine corpus — 1113 of them have no overlapping CES outputs at all —
// untouched by the move (FOLD_PLACEMENT.md 8).
static void verify_single_contributor_is_byte_identical() {
  const Vector asserted{1.0, 0.0};
  Machine m = make_multi_sequence_machine("machine-solo", {{"solo-seq", asserted}}, 20, 2);
  m.metadata["triggerConfig"] = json::parse(R"({"rules":[
    {"sequenceId":"solo-seq","outputMatches":[1,0],"ragStatusCode":"GREEN",
     "processStatus":"ok","description":"solo"}
  ]})");

  PerceptualSpaceSimulator sim(32);
  sim.add_machine(m);
  auto step = sim.process_immediate(fire_at(32));

  assert(step.mergeBatch.size() == 1);
  const auto& op = step.mergeBatch[0];
  assert(op.machineId == "machine-solo");
  // The pre-move engine put the sequence's own asserted values into the
  // operation. Folding a one-element collection under "or" over a binary output
  // returns them unchanged, so this comparison IS the byte-identity claim.
  assert(op.values == asserted);
  assert((op.sequenceIds == std::vector<std::string>{"solo-seq"}));
  assert(op.region.offset == 20 && op.region.length == 2);
  // One computation, not two: what the PE reads and what arbitration receives
  // are the same vector. They were separately computed while the fold sat
  // beside the step.
  const auto& msr = step.machineResults.at("machine-solo");
  assert(msr.mergedOutputVector.has_value());
  assert(*msr.mergedOutputVector == op.values);
  // Governance is that one sequence's own decision, unchanged.
  assert(op.governance.has_value());
  assert(op.governance->sequenceId == "solo-seq");
  assert(op.governance->ragStatusCode == "GREEN");
  assert(op.governance->processStatus == "ok");
  // And it still commits to the space at the machine's output region.
  assert(step.perceptualSpace[20] == 1.0);
  assert(step.perceptualSpace[21] == 0.0);
}

// Governance for a folded contribution is the JOIN over the contributors'
// severity ranks, resolved per sequence against that sequence's own asserted
// values (FOLD_PLACEMENT.md 3). Two things have to hold and neither is implied
// by the other: severity beats enumeration order, and the winner's
// PagingDecision travels WHOLE.
static void verify_governance_join() {
  // GREEN sorts first, RED sorts last. An implementation that took the first
  // resolved decision — or the last — passes one of these cases and fails the
  // other; only the join passes both.
  Machine m = make_multi_sequence_machine("machine-join", {
      {"a-low",  Vector{1.0, 0.0}},
      {"m-mid",  Vector{0.0, 1.0}},
      {"z-high", Vector{1.0, 1.0}},
  }, 20, 2);
  m.metadata["triggerConfig"] = json::parse(R"({"rules":[
    {"sequenceId":"a-low", "outputMatches":[1,0],"ragStatusCode":"GREEN",
     "processStatus":"ok","description":"low","governance":{"ownerTeam":"team-low"}},
    {"sequenceId":"m-mid", "outputMatches":[0,1],"ragStatusCode":"AMBER",
     "processStatus":"warning","description":"mid","governance":{"ownerTeam":"team-mid"}},
    {"sequenceId":"z-high","outputMatches":[1,1],"ragStatusCode":"RED",
     "processStatus":"error","description":"high","governance":{"ownerTeam":"team-high"}}
  ]})");

  PerceptualSpaceSimulator sim(32);
  sim.add_machine(m);
  auto step = sim.process_immediate(fire_at(32));

  // One operation for the machine, carrying all three contributors.
  assert(step.mergeBatch.size() == 1);
  const auto& op = step.mergeBatch[0];
  assert((op.sequenceIds == std::vector<std::string>{"a-low", "m-mid", "z-high"}));
  // "or" over [1,0], [0,1] and [1,1].
  assert((op.values == Vector{1.0, 1.0}));

  assert(op.governance.has_value());
  assert(op.governance->ragStatusCode == "RED");
  // WHOLE, not composed. A record with z-high's ragStatusCode and a-low's
  // ownerTeam describes no rule that exists, and it is a real on-call rota that
  // reads ownerTeam.
  assert(op.governance->sequenceId    == "z-high");
  assert(op.governance->processStatus == "error");
  assert(op.governance->description   == "high");
  assert(op.governance->ownerTeam     == "team-high");

  // Tie — two contributors at the same rank. Broken by the lexicographically
  // smallest sequenceId so the answer cannot depend on enumeration order, which
  // is what differs between runtimes.
  Machine tie = make_multi_sequence_machine("machine-tie", {
      {"a-red", Vector{1.0, 0.0}},
      {"z-red", Vector{0.0, 1.0}},
  }, 20, 2);
  tie.metadata["triggerConfig"] = json::parse(R"({"rules":[
    {"sequenceId":"a-red","outputMatches":[1,0],"ragStatusCode":"RED",
     "processStatus":"error","description":"first-red"},
    {"sequenceId":"z-red","outputMatches":[0,1],"ragStatusCode":"RED",
     "processStatus":"error","description":"second-red"}
  ]})");

  PerceptualSpaceSimulator tieSim(32);
  tieSim.add_machine(tie);
  auto tieStep = tieSim.process_immediate(fire_at(32));
  assert(tieStep.mergeBatch.size() == 1);
  const auto& tieOp = tieStep.mergeBatch[0];
  assert((tieOp.sequenceIds == std::vector<std::string>{"a-red", "z-red"}));
  assert(tieOp.governance.has_value());
  assert(tieOp.governance->sequenceId  == "a-red");
  assert(tieOp.governance->description == "first-red");

  // No contributor resolving means no decision — not an empty one.
  Machine bare = make_multi_sequence_machine("machine-bare", {{"only", Vector{1.0, 0.0}}}, 20, 2);
  PerceptualSpaceSimulator bareSim(32);
  bareSim.add_machine(bare);
  auto bareStep = bareSim.process_immediate(fire_at(32));
  assert(bareStep.mergeBatch.size() == 1);
  assert(!bareStep.mergeBatch[0].governance.has_value());
}

// The event bus keys subscriptions on machineId + "|" + sequenceId and writes
// into the perceptual space. A folded operation has no single sequenceId, so an
// implementation that reads one scalar either matches nothing — meta machines
// never fire — or matches one arbitrarily chosen member. Every contributing
// sequence must still fire its subscriptions (FOLD_PLACEMENT.md 5).
static void verify_event_bus_fires_for_every_contributor() {
  Machine producer = make_multi_sequence_machine("machine-producer", {
      {"seq-alpha", Vector{1.0, 0.0}},
      {"seq-beta",  Vector{0.0, 1.0}},
  }, 20, 2);

  // A subscriber with no sequences of its own: it exists to declare the two
  // subscriptions and to be somewhere for the latched bits to land.
  PerceptualMapping subMapping{{30, 2}, {40, 2}};
  Machine subscriber("Subscriber", "event bus test", ArbiterRule::Passthrough, subMapping,
                     "machine-subscriber");
  subscriber.metadata["compose"] = json::parse(R"({"subscriptions":[
    {"producerMachineId":"machine-producer","producerSequenceId":"seq-alpha","bitOffset":30},
    {"producerMachineId":"machine-producer","producerSequenceId":"seq-beta", "bitOffset":31}
  ]})");

  PerceptualSpaceSimulator sim(64);
  sim.add_machine(producer);
  sim.add_machine(subscriber);
  auto step = sim.process_immediate(fire_at(64));

  // One merge operation, two contributors, two event-bus writes.
  assert(step.mergeBatch.size() == 1);
  assert(step.mergeBatch[0].sequenceIds.size() == 2);
  assert(step.eventBus.size() == 2);
  assert(step.eventBus[0].producerSequenceId == "seq-alpha");
  assert(step.eventBus[0].bitOffset == 30);
  assert(step.eventBus[1].producerSequenceId == "seq-beta");
  assert(step.eventBus[1].bitOffset == 31);
  // The writes reached the space — this is the consumer whose behaviour has to
  // be identical rather than analogous.
  assert(step.perceptualSpace[30] == 1.0);
  assert(step.perceptualSpace[31] == 1.0);
}

// A refusal is the machine presenting NOTHING, and nothing is not zeros. The
// Łukasiewicz pair is undefined without a declared chain top and refuses rather
// than guessing one; the machine then contributes no operation for the region,
// exactly as if it had completed no Reality Event (FOLD_PLACEMENT.md 2).
// Contributing a zero vector instead would have the arbiter resolve — and the
// PE clear — a region no machine wrote.
static void verify_fold_refusal_contributes_nothing() {
  Machine m = make_multi_sequence_machine("machine-refuse", {
      {"a-seq", Vector{1.0, 0.0}},
      {"b-seq", Vector{0.0, 1.0}},
  }, 20, 2);
  // No outputAlphabetTop declared. No corpus machine declares one either — the
  // two that select a chain fold both select "join", which needs no top — so
  // the refusal path is reachable only by a machine that asks for the
  // Łukasiewicz pair without saying what its chain is.
  m.outputMergeTransformation = OutputMergeTransformation::StrongDisjunction;

  PerceptualSpaceSimulator sim(32);
  sim.add_machine(m);
  auto step = sim.process_immediate(fire_at(32));

  assert(step.mergeBatch.empty());
  const auto& msr = step.machineResults.at("machine-refuse");
  assert(!msr.mergedOutputVector.has_value());
  // The sequences DID complete Reality Events — the arbiter's pick is still
  // reported — but the machine presents nothing, so nothing is written.
  assert(msr.outputVector.has_value());
  assert(step.perceptualSpace[20] == 0.0);
  assert(step.perceptualSpace[21] == 0.0);

  // Declaring the chain top makes the same machine contribute normally, which
  // is what confirms the refusal was about k and not about the machine. The top
  // lives on the perceptual mapping, where machine.schema.json puts it.
  m.perceptualMapping->outputAlphabetTop = 1;
  PerceptualSpaceSimulator declared(32);
  declared.add_machine(m);
  auto ok = declared.process_immediate(fire_at(32));
  assert(ok.mergeBatch.size() == 1);
  assert((ok.mergeBatch[0].values == Vector{1.0, 1.0}));
  assert((ok.mergeBatch[0].sequenceIds == std::vector<std::string>{"a-seq", "b-seq"}));
}

// A refusal withdraws the machine's VALUE, not its firings.
//
// §2 says a refusing fold contributes no MergeOperation; §5 says every
// (producer machine, producer sequence) subscription that would have fired must
// still fire. Both hold only if the event bus is driven by which CESs
// completed rather than by mergeBatch — read off the batch, a refusal silently
// retracts the producer's firings and its meta machines never see it fire.
static void verify_refusal_still_fires_the_event_bus() {
  Machine producer = make_multi_sequence_machine("machine-refuse-bus", {
      {"seq-alpha", Vector{1.0, 0.0}},
      {"seq-beta",  Vector{0.0, 1.0}},
  }, 20, 2);
  // Refuses: the Łukasiewicz pair with no declared chain top.
  producer.outputMergeTransformation = OutputMergeTransformation::StrongConjunction;

  PerceptualMapping subMapping{{30, 2}, {40, 2}};
  Machine subscriber("Subscriber", "refusal event bus test", ArbiterRule::Passthrough, subMapping,
                     "machine-refuse-sub");
  subscriber.metadata["compose"] = json::parse(R"({"subscriptions":[
    {"producerMachineId":"machine-refuse-bus","producerSequenceId":"seq-alpha","bitOffset":30},
    {"producerMachineId":"machine-refuse-bus","producerSequenceId":"seq-beta", "bitOffset":31}
  ]})");

  PerceptualSpaceSimulator sim(64);
  sim.add_machine(producer);
  sim.add_machine(subscriber);
  auto step = sim.process_immediate(fire_at(64));

  // No value presented — nothing reaches arbitration, and the output region is
  // untouched.
  assert(step.mergeBatch.empty());
  assert(!step.machineResults.at("machine-refuse-bus").mergedOutputVector.has_value());
  assert(step.perceptualSpace[20] == 0.0);
  assert(step.perceptualSpace[21] == 0.0);

  // But both Reality Events completed, so both subscriptions fire and both bits
  // latch. This is the assertion that fails if the bus is read off mergeBatch.
  assert(step.eventBus.size() == 2);
  assert(step.eventBus[0].producerSequenceId == "seq-alpha");
  assert(step.eventBus[1].producerSequenceId == "seq-beta");
  assert(step.perceptualSpace[30] == 1.0);
  assert(step.perceptualSpace[31] == 1.0);
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
    // Two machines contending for cell 20, asserting 0.2 and 0.8. This used to
    // resolve to 0.8: the arbiter saw the raw asserted values, because the fold
    // sat beside the machine's step rather than in it.
    //
    // It resolves to 1.0 now, and the reason is worth stating rather than
    // patching. Neither machine declares an outputMergeTransformation, so both
    // fold under the default "or" — and "or" is a Boolean gate defined over
    // assertion, not magnitude, so 0.2 and 0.8 both leave the fold as 1.0.
    // Moving the fold in front of arbitration therefore binarises every
    // non-binary asserted output in the corpus.
    //
    // That is FOLD_PLACEMENT.md 2 meeting the default transformation, and it is
    // why the two corpus machines carrying a severity ladder over {0..4} —
    // FallDetection and FallSensorMotionPreaggregator — now declare
    // `outputMergeTransformation: "join"`. join is max over the chain, so their
    // ladders survive the fold intact. The remaining 1326 machines assert only
    // 0 and 1, where "or" is already the identity on a single contributor.
    //
    // These two synthetic machines declare nothing and so still fold under "or",
    // which is what this assertion pins: the default binarises, and a machine
    // whose outputs carry magnitude has to say so.
    assert(step.perceptualSpace[20] == 1.0);
  }

  {
    // Boolean gates — unchanged behaviour. All 1273 corpus machines fold with
    // these and none of them declares a chain top, so this block is the
    // regression that the multi-valued transformations were added beside the
    // Boolean path rather than through it.
    const std::vector<Vector> none{};
    // No completed Reality Event means no output presented, which is not the
    // same as presenting zeros — the PE would clear a region no machine wrote.
    assert(!fold_outputs(none, OutputMergeTransformation::Or).has_value());
    assert(!fold_outputs(none, OutputMergeTransformation::Meet).has_value());

    const std::vector<Vector> mixed{{1.0, 1.0, 0.0}, {1.0, 0.0, 0.0}};
    assert(*fold_outputs(mixed, OutputMergeTransformation::Or)   == Vector({1.0, 1.0, 0.0}));
    assert(*fold_outputs(mixed, OutputMergeTransformation::And)  == Vector({1.0, 0.0, 0.0}));
    assert(*fold_outputs(mixed, OutputMergeTransformation::Xor)  == Vector({0.0, 1.0, 0.0}));
    assert(*fold_outputs(mixed, OutputMergeTransformation::Nor)  == Vector({0.0, 0.0, 1.0}));
    assert(*fold_outputs(mixed, OutputMergeTransformation::Nand) == Vector({0.0, 1.0, 1.0}));

    // Widest contributor wins the width; a short one contributes 0 past its
    // end rather than truncating its peers.
    const std::vector<Vector> ragged{{1.0}, {0.0, 1.0}};
    assert(*fold_outputs(ragged, OutputMergeTransformation::Or) == Vector({1.0, 1.0}));

    // Names round-trip, including the multi-valued ones the API offers.
    for (const auto* name : {"or", "and", "xor", "nor", "nand", "meet", "join",
                             "strong-conjunction", "strong-disjunction",
                             "discrete-median"}) {
      assert(to_string(output_merge_from_string(name)) == std::string(name));
    }
    bool rejectedName = false;
    try { (void)output_merge_from_string("median"); }
    catch (const std::invalid_argument&) { rejectedName = true; }
    assert(rejectedName);
  }

  {
    // Worked examples from the specification, chain {0..3}.
    assert(fold_scalar({1, 1, 1}, OutputMergeTransformation::StrongDisjunction, 3) == 3);
    assert(fold_scalar({3, 3, 2}, OutputMergeTransformation::StrongConjunction, 3) == 2);
    assert(fold_scalar({3, 3, 0}, OutputMergeTransformation::StrongConjunction, 3) == 0);
    assert(fold_scalar({3, 3, 0, 3}, OutputMergeTransformation::DiscreteMedian, 3) == 3);
    assert(fold_scalar({3, 1, 2}, OutputMergeTransformation::Meet, 3) == 1);
    assert(fold_scalar({3, 1, 2}, OutputMergeTransformation::Join, 3) == 3);

    // Even n takes floor(median) — the lower of the two middle elements, by
    // selection and never by division.
    assert(fold_scalar({0, 1, 2, 3}, OutputMergeTransformation::DiscreteMedian, 3) == 1);

    // Not idempotent, deliberately: ⊕ saturates and ⊙ extinguishes.
    assert(fold_scalar({1, 1}, OutputMergeTransformation::StrongDisjunction, 3) == 2);
    assert(fold_scalar({1, 1}, OutputMergeTransformation::StrongConjunction, 3) == 0);

    verify_mv_transform_properties();
    verify_de_morgan_duality();

    // FallDetection's severity ladder — the machine that exposed the problem.
    // It ranges over {0..4} while declaring 4 bits, so k=15 (the representable
    // maximum) would put ⊕ at 14, outside the alphabet entirely. That is why k
    // is a parameter and not derived from bitsPerElement.
    const std::vector<long long> ladder{0, 1, 2, 3, 4, 4, 0};
    assert(fold_scalar(ladder, OutputMergeTransformation::StrongDisjunction, 4) == 4);
    assert(fold_scalar(ladder, OutputMergeTransformation::StrongDisjunction, 15) == 14);
    // Meet, join and median take no k and are unaffected by the question.
    for (const std::optional<int> k : {std::optional<int>(4), std::optional<int>(15),
                                       std::optional<int>()}) {
      assert(fold_scalar(ladder, OutputMergeTransformation::Meet, k) == 0);
      assert(fold_scalar(ladder, OutputMergeTransformation::Join, k) == 4);
      assert(fold_scalar(ladder, OutputMergeTransformation::DiscreteMedian, k) == 2);
    }

    // Absent chain top: the Łukasiewicz pair REFUSES rather than assuming one.
    //
    // A Boolean-chain fallback (k = 1) was tried and is unsound in both
    // directions. ⊕ clamps, so the ladder above folds to 1 — the flattening
    // this vocabulary exists to prevent, reintroduced through the parameter.
    // ⊙ does not clamp: its closure bound holds only while every xᵢ ≤ k, so
    // the same ladder at k = 1 yields 8, outside both the chain {0,1} and the
    // machine's alphabet {0..4}. Smallest witness: k = 1 over [2,2] gives 3.
    //
    // Refusal is the only answer that neither flattens nor fabricates, and it
    // is what Scala and LSP do, so the three runtimes cannot diverge here.
    const std::vector<Vector> binary{{1.0, 1.0, 0.0}, {1.0, 0.0, 0.0}};
    assert(!fold_outputs(binary, OutputMergeTransformation::StrongDisjunction).has_value());
    assert(!fold_outputs(binary, OutputMergeTransformation::StrongConjunction).has_value());
    // With a chain top they fold normally, including on binary input.
    assert(*fold_outputs(binary, OutputMergeTransformation::StrongDisjunction, 1) ==
           *fold_outputs(binary, OutputMergeTransformation::Or));
    assert(*fold_outputs(binary, OutputMergeTransformation::StrongConjunction, 1) ==
           *fold_outputs(binary, OutputMergeTransformation::And));
    // The witness itself: k = 1 over inputs above the chain violates closure,
    // which is why no degenerate default is safe.
    assert(*fold_outputs({{2.0}, {2.0}}, OutputMergeTransformation::StrongConjunction, 1)
           == Vector({3.0}));
    // Meet, Join and DiscreteMedian take no k and are unaffected by its absence
    // — asserted above across k = 4, 15 and absent.

    // join clamps each read to k, so a contribution above the chain cannot let
    // arrival order decide the answer. Without the clamp the early exit stopped
    // the scan at whatever arrived first: [1,3,5] at k=3 gave 3 and [5,3,1]
    // gave 5. Off-chain input is a malformed corpus, but a fold whose answer
    // depends on ordering is exactly what three runtimes must not have.
    assert(*fold_outputs({{1.0}, {3.0}, {5.0}}, OutputMergeTransformation::Join, 3)
           == Vector({3.0}));
    assert(*fold_outputs({{5.0}, {3.0}, {1.0}}, OutputMergeTransformation::Join, 3)
           == Vector({3.0}));
    assert(*fold_outputs({{3.0}, {5.0}, {1.0}}, OutputMergeTransformation::Join, 3)
           == Vector({3.0}));
    // Unbounded, join is still the plain maximum.
    assert(*fold_outputs({{5.0}, {3.0}, {1.0}}, OutputMergeTransformation::Join)
           == Vector({5.0}));

    // Conversion from the engine's doubles is by rounding, not truncation: a
    // cell an arbiter scaled to just under its integer must not fold a rung
    // lower on one runtime than on another that accumulated the scaling
    // differently. Negatives and NaN floor at the chain bottom.
    const std::vector<Vector> scaled{{2.9999999996}, {3.0}};
    assert(*fold_outputs(scaled, OutputMergeTransformation::Meet, 4) == Vector({3.0}));
    const std::vector<Vector> degenerate{{-1.0}, {std::nan("")}, {2.0}};
    assert(*fold_outputs(degenerate, OutputMergeTransformation::Join, 4) == Vector({2.0}));
    assert(*fold_outputs(degenerate, OutputMergeTransformation::Meet, 4) == Vector({0.0}));
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

  {
    // The fold's move into the machine's atomic step.
    verify_single_contributor_is_byte_identical();
    verify_governance_join();
    verify_event_bus_fires_for_every_contributor();
    verify_fold_refusal_contributes_nothing();
    verify_refusal_still_fires_the_event_bus();
  }

  std::cout << "RealityEngine_CPP smoke tests passed\n";
  return 0;
}
