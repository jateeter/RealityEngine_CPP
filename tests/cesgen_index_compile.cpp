// Compile-only verification that every generated header in
// `include/reality/generated/` is syntactically valid C++ and that the
// aggregate `index.hpp` resolves without error.  This is the C++ side of
// the cesgen drift gate — paired with src/__tests__/CesgenVerification.test.ts
// in the active Scala replacement and shared machine corpus.
//
// We include the index, then sample a constexpr value from each of the
// well-known machines so the compiler instantiates and resolves each
// namespace.  Adding a new machine to the corpus does not require this
// file to change; the index include and the existence checks below cover
// it transitively.

#include "reality/generated/index.hpp"

#include <iostream>
#include <string_view>

int main() {
  // Spot-check named test machines.
  static_assert(reality::generated::rsflipflop::input_length == 2);
  static_assert(reality::generated::rsflipflop::output_length == 2);
  static_assert(reality::generated::rs2::output_length == 2);
  static_assert(reality::generated::multistep::input_length == 3);
  static_assert(reality::generated::dlx001_rising_edge_detector::input_length == 4);

  // Round-trip enum to string.
  constexpr auto rs_set_name = reality::generated::rsflipflop::sequence_id_string(
      reality::generated::rsflipflop::SequenceId::RsSetSequence);
  static_assert(rs_set_name == std::string_view{"rs-set-sequence"});

  std::cout << "cesgen index compile test passed\n";
  return 0;
}
