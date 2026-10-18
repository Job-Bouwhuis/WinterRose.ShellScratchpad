#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace ShellScratchpad::Util
{
struct FuzzyMatch
{
    QString candidate;
    int distance = 0;
};

// Returns close candidates ordered by edit distance, then case-insensitive name.
QVector<FuzzyMatch> fuzzyMatches(const QString &query, const QStringList &candidates,
                                 int maximumDistance = 2, int maximumResults = 10);

// Returns a deterministic fuzzy score, or -1 when the candidate is too distant.
// Lower scores are better. Exact and prefix matches are intentionally handled by callers.
int fuzzyMatchScore(const QString &query, const QString &candidate);
}
