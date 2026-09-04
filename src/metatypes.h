#pragma once

#include <QMetaType>
#include <silent.h>

// Register CXX shared types with Qt's meta-object system
// for cross-thread signal-slot connections (queued delivery).
Q_DECLARE_METATYPE(BackendInfo)
Q_DECLARE_METATYPE(ConnectionResult)
Q_DECLARE_METATYPE(TxResult)
Q_DECLARE_METATYPE(Notification)
Q_DECLARE_METATYPE(HwiEvent)
Q_DECLARE_METATYPE(HwiDevice)
