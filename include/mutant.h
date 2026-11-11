#pragma once

// Seeded bugs for the checker to find. The build chooses one with
// -DMUTANT_ID=n. Mutant 0 is the correct store. docs/mutants.md lists them.

#ifndef MUTANT_ID
#define MUTANT_ID 0
#endif

namespace csw {

constexpr bool mutant_is(int id) { return MUTANT_ID == id; }

}  // namespace csw
