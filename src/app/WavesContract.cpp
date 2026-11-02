// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/WavesContract.h"

#include "CardContract.h"

// The contract's. The job that opens the file takes this over, beside it, as
// app/AutoDj has autoDjReadMpdj(), and current() then says major 1, with
// tools/card_fixtures.py's device_main() and test_card_contract's asserts.
uint32_t wavesReadMpwf() { return cardcontract::devicetxt::current("").readMpwf; }
