#pragma once

#include <QString>
#include <silent.h>

// What a backend failure of `kind` is summed up as.
auto mapBackendErrorSummary(BackendErrorKind kind) -> QString;
