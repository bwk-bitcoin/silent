use silent::{normalize_fee_rate, parse_btc_amount, parse_fee_input};

#[test]
fn exact_btc_amounts() {
    for (text, sats) in [
        ("0.00000001", 1),
        (".00000003", 3),
        ("0.29", 29_000_000),
        ("1.", 100_000_000),
        ("1.23456789", 123_456_789),
        ("21000000", 2_100_000_000_000_000),
    ] {
        let result = parse_btc_amount(text);
        assert!(result.is_ok, "valid BTC text {text}");
        assert_eq!(result.sats, sats, "exact sats for {text}");
    }
    for text in [
        "",
        ".",
        "0",
        "-1",
        "+1",
        "1e2",
        "NaN",
        "inf",
        "1,5",
        " 1",
        "1 ",
        "0.000000001",
        "21000000.00000001",
        "18446744073709551616",
    ] {
        let result = parse_btc_amount(text);
        assert!(!result.is_ok, "reject BTC text {text:?}");
        assert_eq!(result.sats, 0, "no usable value for {text:?}");
    }
}

#[test]
fn exact_fees() {
    for (text, rate) in [
        ("0.001", 0.001),
        ("1.234", 1.234),
        ("1.", 1.0),
        ("1000000", 1_000_000.0),
    ] {
        let result = parse_fee_input(text, true);
        assert!(result.is_ok, "valid rate {text}");
        assert_eq!(result.fee_rate, rate, "rate for {text}");
        assert_eq!(result.fee, 0, "rate is not an absolute fee");
    }
    for text in ["", "0", "0.0001", "1.0001", "-1", "NaN", "1e2", "1,2"] {
        assert!(!parse_fee_input(text, true).is_ok, "reject rate {text:?}");
    }
    let fee = parse_fee_input("123456789", false);
    assert!(fee.is_ok, "integer absolute fee");
    assert_eq!(fee.fee, 123_456_789, "exact absolute fee");
    assert_eq!(fee.fee_rate, 0.0, "absolute fee has no entered rate");
    for text in [
        "",
        "0",
        "1.0",
        "1.5",
        "-1",
        "1e3",
        "2100000000000001",
        "18446744073709551616",
    ] {
        assert!(
            !parse_fee_input(text, false).is_ok,
            "reject absolute fee {text:?}"
        );
    }
}

#[test]
fn provider_fee_rates_use_the_builder_conversion() {
    for (quote, expected) in [(0.1234567, 0.123), (0.1236, 0.124), (1.0000001, 1.0)] {
        let result = normalize_fee_rate(quote);
        assert!(result.is_ok, "valid provider rate {quote}");
        assert_eq!(
            result.fee_rate, expected,
            "canonical msat/vB for provider rate {quote}"
        );
    }
    for quote in [0.0, -1.0, 0.0001, f64::NAN, f64::INFINITY] {
        assert!(
            !normalize_fee_rate(quote).is_ok,
            "invalid provider rate {quote}"
        );
    }
}
