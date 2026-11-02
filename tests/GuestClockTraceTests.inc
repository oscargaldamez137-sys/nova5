void CheckGuestClockScale() {
  constexpr const char* name = "GuestClockScale";
  uint32_t percent = 0;
  Require(name, "default", GuestClockTestAccess::ParseScale(nullptr, percent) && percent == 100,
          "absent scale did not preserve the original clock");
  for (const auto text : {"", "9", "201", "0", "-50", "+50", "50.5", "50x", " 50", "50 ",
                          "99999999999999999999999"}) {
    percent = 100;
    Require(name, "invalid option", !GuestClockTestAccess::ParseScale(text, percent) && percent == 100,
            "invalid scale accepted or changed the default");
  }
  constexpr uint64_t base = 0xf000000000000000ull;
  const auto scale = [&](uint64_t value, uint64_t anchor, uint32_t factor) {
    uint64_t result = 0;
    Require(name, "conversion", GuestClockTestAccess::Scale(value, anchor, factor, result),
            "valid timestamp was rejected");
    return result;
  };
  for (uint32_t factor = 10; factor <= 200; ++factor) {
    const auto text = std::to_string(factor);
    Require(name, "option", GuestClockTestAccess::ParseScale(text.c_str(), percent) && percent == factor,
            "valid percentage parsed incorrectly");
    // Interleaved pairs use one epoch, even at values too large for exact floating-point integers.
    const auto begin_a = scale(base + 100000000, base, factor);
    const auto begin_b = scale(base + 101000000, base, factor);
    const auto end_a = scale(base + 103000000, base, factor);
    const auto end_b = scale(base + 107000000, base, factor);
    Require(name, "intervals", end_a - begin_a == 30000ull * factor &&
        end_b - begin_b == 60000ull * factor && scale(base, base, factor) == base,
        "pairs used different scales or the session base moved");
    uint64_t previous = 0;
    for (int offset = -100; offset <= 100; ++offset) {
      const auto value = offset < 0 ? base - uint64_t(-offset) : base + uint64_t(offset);
      const auto actual = scale(value, base, factor);
      const int product = offset * static_cast<int>(factor);
      const int ticks = product >= 0 ? product / 100 : -((-product + 99) / 100);
      const auto expected = ticks < 0 ? base - uint64_t(-ticks) : base + uint64_t(ticks);
      Require(name, "rounding", actual == expected && actual >= previous &&
          (factor != 100 || actual == value), "clock lost monotonicity, precision or identity at 100");
      previous = actual;
    }
  }
  Require(name, "limits", scale(UINT64_MAX, 0, 100) == UINT64_MAX &&
      scale(0, UINT64_MAX, 100) == 0 &&
      scale(UINT64_MAX, 0, 10) == UINT64_MAX / 10 &&
      scale(UINT64_MAX / 2, 0, 200) == UINT64_MAX - 1,
      "safe extreme values lost precision");
  uint64_t result = 123;
  Require(name, "overflow", !GuestClockTestAccess::Scale(UINT64_MAX, 0, 200, result) &&
      !GuestClockTestAccess::Scale(0, UINT64_MAX, 200, result) &&
      !GuestClockTestAccess::Scale(UINT64_MAX, UINT64_MAX - 1, 200, result) &&
      !GuestClockTestAccess::Scale(0, 1, 200, result) &&
      !GuestClockTestAccess::Scale(base, base, 9, result) &&
      !GuestClockTestAccess::Scale(base, base, 201, result) && result == 123,
      "invalid conversion wrapped or modified its output");
  Require(name, "native wrap", GuestClock::Convert(0x2c, 0xff00, base, 0xffff, 10.0, result) &&
      scale(result, base, 50) == base + 150, "native wrap was scaled before clock conversion");
  if (GuestClock::Enabled()) {
    const auto options = GuestClockTestAccess::ScaleOptions();
    const auto first = GuestClock::AdjustTimestamp(options.base + 100000000);
    const auto second = GuestClock::AdjustTimestamp(options.base + 103000000);
    Require(name, "configured clock", first == options.base + 1000000ull * options.percent &&
        second - first == 30000ull * options.percent,
        "publication transform did not use the configured percentage and fixed base");
  } else {
    Require(name, "CPU mode", GuestClock::AdjustTimestamp(base + 123) == base + 123,
            "native-clock opt-out still scaled timestamps");
  }
  std::printf("[clock]   %s ok\n", name);
}

void CheckGuestClockPairTrace() {
  using Trace = GuestClockPairTrace;
  using Source = Trace::Source;
  constexpr const char *name = "GuestClockPairTrace";
  Trace trace;
  trace.Add(0x1000, 0x1100);
  trace.Add(0x2000, 0x2100);

  trace.Begin(0x1000);
  trace.Begin(0x2000);
  trace.Begin(0x1100);
  trace.Written(0x1100, Source::Cpu);
  Require(name, "pending begin", trace.Snapshot().mixed == 0 && trace.Snapshot().unresolved == 1,
          "an unpublished begin was classified as CPU or GPU");
  trace.Written(0x2000, Source::Cpu);
  trace.Written(0x1000, Source::Gpu);
  Require(name, "mixed completion", trace.Snapshot().mixed == 1 && trace.Snapshot().unresolved == 0,
          "a GPU begin and CPU end were not counted once");
  trace.Written(0x1000, Source::Gpu);
  Require(name, "deduplication", trace.Snapshot().mixed == 1, "one pair was counted twice");

  trace.Begin(0x2100);
  trace.Written(0x2100, Source::Cpu);
  Require(name, "interleaved timer", trace.Snapshot().cpu == 1 && trace.Snapshot().mixed == 1,
          "interleaved timers were paired with each other");

  trace.Begin(0x1000);
  trace.Written(0x1000, Source::Gpu);
  trace.Begin(0x1100);
  trace.Written(0x1100, Source::Gpu);
  Require(name, "next cycle", trace.Snapshot().gpu == 1 && trace.Snapshot().mixed == 1,
          "the reused slot retained its previous source");

  trace.Begin(0x1000);
  trace.Written(0x1000, Source::Gpu);
  trace.Cancel(0x1004, 4);
  trace.Begin(0x1100);
  trace.Written(0x1100, Source::Cpu);
  Require(name, "overwritten begin", trace.Snapshot().mixed == 1 && trace.Snapshot().unresolved == 1,
          "a canceled begin was counted as a valid GPU write");

  trace.Begin(0x1000);
  trace.Written(0x1000, Source::Gpu);
  Require(name, "old end", trace.Snapshot().mixed == 1, "an old end was paired with a new begin");
  std::printf("[clock]   %s ok\n", name);
}
