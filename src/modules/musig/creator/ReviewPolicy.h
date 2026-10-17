#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <cstdint>
#include <silent.h>

namespace catalog {
class CopyField;
class Label;
} // namespace catalog

namespace qontrol {
class Column;
} // namespace qontrol

namespace musig::modal {

// Who signs one path and when, in plain words. `names` are the signer names in
// path order, `mandatory` flags, at the same index, the ones that sign every spend
// (MultiMandatory).
[[nodiscard]] auto pathWords(PolicySemantic semantic, std::uint32_t threshold,
                             PolicyLocktime locktime, std::uint32_t locktime_value,
                             const QStringList &names, const QList<bool> &mandatory) -> QString;

// Step 4: plain words from a PolicyReview, the leaf count and the descriptor.
class ReviewPolicy : public QWidget {
    Q_OBJECT

public:
    explicit ReviewPolicy(QWidget *parent = nullptr);
    // Fingerprint to alias, for the names of the signers.
    void setAliases(const QHash<QString, QString> &aliases);
    void setReview(const PolicyReview &review, const QString &descriptor);

protected:
    void init();
    void view();
    [[nodiscard]] auto pathText(const PolicyReviewPath &path) const -> QString;
    [[nodiscard]] auto keyName(const QString &fingerprint) const -> QString;
    [[nodiscard]] auto pathCard(int index, const PolicyReviewPath &path) const -> QWidget *;

private:
    qontrol::Column *m_paths_column = nullptr;
    catalog::Label *m_leaves_label = nullptr;
    catalog::CopyField *m_descriptor_field = nullptr;
    catalog::Label *m_checksum_label = nullptr;
    QHash<QString, QString> m_aliases;
};

} // namespace musig::modal
