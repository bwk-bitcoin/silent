#include "account/Money.h"

#include <cstdint>
#include <silent.h>

namespace account::money {
namespace {

auto toVec(const QList<quint64> &values) -> rust::Vec<std::uint64_t> {
    rust::Vec<std::uint64_t> vec;
    for (auto value : values) {
        vec.push_back(value);
    }
    return vec;
}

} // namespace

auto amounts(const QList<quint64> &inputs, const QList<quint64> &recipients, quint64 fee)
    -> Amounts {
    auto in = toVec(inputs);
    auto out = toVec(recipients);
    auto result = ::send_amounts(rust::Slice<const std::uint64_t>(in.data(), in.size()),
                                 rust::Slice<const std::uint64_t>(out.data(), out.size()), fee);
    return {.inputTotal = result.input_total,
            .sendingTotal = result.sending_total,
            .remaining = result.remaining,
            .change = result.change,
            .transactionTotal = result.transaction_total};
}

auto parseBtc(const QString &btc) -> std::optional<quint64> {
    auto text = btc.toStdString();
    auto parsed = ::parse_btc_amount(rust::Str(text));
    if (!parsed.is_ok) {
        return std::nullopt;
    }
    return parsed.sats;
}

auto parseFee(const QString &text, bool rate) -> std::optional<Fee> {
    auto input = text.toStdString();
    auto parsed = ::parse_fee_input(rust::Str(input), rate);
    if (!parsed.is_ok) {
        return std::nullopt;
    }
    return Fee{.rate = parsed.fee_rate, .absolute = parsed.fee};
}

auto normalizeFeeRate(double rate) -> std::optional<double> {
    auto normalized = ::normalize_fee_rate(rate);
    if (!normalized.is_ok) {
        return std::nullopt;
    }
    return normalized.fee_rate;
}

auto isAddress(const QString &address) -> bool {
    return ::validate_address(rust::String(address.toStdString())).empty();
}

} // namespace account::money
