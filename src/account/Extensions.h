#pragma once

#include <QString>

// The value extension operations of the built-in SP and Multikey providers that the shared
// account views use when a provider answers them. A provider that does not answers error(),
// and the view goes without what the extension adds.
namespace account::extension {

// IBlockchain::value(), payload the txid: what the details of a fetched transaction leave
// out, as {"version": int, "lockTime": int, "chainTip": int (absent when unknown),
// "scripts": {"<output index>": "<script>"} for the outputs that pay no address}.
inline const QString TRANSACTION_EXTRAS = QStringLiteral("silent.transactionExtras");

// IBackendSettings::value(), payload the endpoint id: what that server reports it serves, as
// {"tweaksOnly", "tweaksFullBasic", "tweaksFullWithDustFilter",
// "tweaksCutThroughWithDustFilter"}, each a bool.
inline const QString ENDPOINT_CAPABILITIES = QStringLiteral("silent.endpointCapabilities");

// IBackendSettings::value(), no payload: how Settings shows each endpoint, as {"<endpoint
// id>": {"label", "placeholder", "connect", "disconnect": text in the current language,
// "transport": bool (SSL and certificate toggles), "capabilities": bool
// (ENDPOINT_CAPABILITIES under its row)}}. An endpoint left out shows by its name.
inline const QString ENDPOINT_PRESENTATION = QStringLiteral("silent.endpointPresentation");

// IAccountBackend::value(), no payload: the title of the list of each address kind's
// addresses awaiting payment, as {"<kind id>": text in the current language}. A kind left
// out is titled by its name.
inline const QString ADDRESS_KIND_TITLES = QStringLiteral("silent.addressKindTitles");

// IBackendSettings::value(), no payload: whether an endpoint operation (a test, a save, a
// connection change) runs, as a bool. The same value comes as a valueEvent() of this name
// whenever it changes.
inline const QString ENDPOINTS_BUSY = QStringLiteral("silent.endpointsBusy");

} // namespace account::extension
