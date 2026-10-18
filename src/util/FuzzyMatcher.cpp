#include "FuzzyMatcher.h"

#include <QStringList>

#include <algorithm>

namespace ShellScratchpad::Util
{
namespace
{
int editDistance(const QString &left, const QString &right, int cutoff)
{
    const QString a = left.toCaseFolded();
    const QString b = right.toCaseFolded();
    if (qAbs(a.size() - b.size()) > cutoff)
        return cutoff + 1;

    QVector<QVector<int>> distance(a.size() + 1, QVector<int>(b.size() + 1));
    for (qsizetype row = 0; row <= a.size(); ++row)
        distance[row][0] = static_cast<int>(row);
    for (qsizetype column = 0; column <= b.size(); ++column)
        distance[0][column] = static_cast<int>(column);
    for (qsizetype row = 1; row <= a.size(); ++row) {
        for (qsizetype column = 1; column <= b.size(); ++column) {
            const int substitution = distance[row - 1][column - 1] +
                (a.at(row - 1) == b.at(column - 1) ? 0 : 1);
            distance[row][column] = qMin(qMin(distance[row - 1][column] + 1,
                distance[row][column - 1] + 1), substitution);
            if (row > 1 && column > 1 && a.at(row - 1) == b.at(column - 2) &&
                a.at(row - 2) == b.at(column - 1))
                distance[row][column] = qMin(distance[row][column], distance[row - 2][column - 2] + 1);
        }
    }
    return distance[a.size()][b.size()];
}

int subsequenceScore(const QString &query, const QString &candidate)
{
    if (query.size() < 3 || candidate.size() > query.size() * 4 + 5)
        return -1;
    const QString q = query.toCaseFolded();
    const QString c = candidate.toCaseFolded();
    constexpr int UNREACHABLE = 1000000;
    QVector<QVector<int>> scores(q.size(), QVector<int>(c.size(), UNREACHABLE));
    for (qsizetype candidateIndex = 0; candidateIndex < c.size(); ++candidateIndex) {
        if (c.at(candidateIndex) == q.at(0))
            scores[0][candidateIndex] = static_cast<int>(candidateIndex) * 12;
    }
    for (qsizetype queryIndex = 1; queryIndex < q.size(); ++queryIndex) {
        int bestGapPrefix = UNREACHABLE;
        for (qsizetype candidateIndex = 1; candidateIndex < c.size(); ++candidateIndex) {
            if (candidateIndex >= 2) {
                const qsizetype previousIndex = candidateIndex - 2;
                if (scores[queryIndex - 1][previousIndex] != UNREACHABLE)
                    bestGapPrefix = qMin(bestGapPrefix,
                        scores[queryIndex - 1][previousIndex] - 12 * static_cast<int>(previousIndex));
            }
            if (c.at(candidateIndex) != q.at(queryIndex))
                continue;
            const int consecutive = scores[queryIndex - 1][candidateIndex - 1];
            if (consecutive != UNREACHABLE)
                scores[queryIndex][candidateIndex] = consecutive;
            if (bestGapPrefix != UNREACHABLE)
                scores[queryIndex][candidateIndex] = qMin(scores[queryIndex][candidateIndex],
                    bestGapPrefix + 8 + 12 * static_cast<int>(candidateIndex));
        }
    }
    int best = UNREACHABLE;
    for (int score : scores.constLast())
        best = qMin(best, score);
    if (best == UNREACHABLE)
        return -1;
    return 1000 + best + static_cast<int>(candidate.size() - query.size()) * 3;
}
}

QVector<FuzzyMatch> fuzzyMatches(const QString &query, const QStringList &candidates,
                                 int maximumDistance, int maximumResults)
{
    QVector<FuzzyMatch> matches;
    const QString normalizedQuery = query.trimmed();
    if (normalizedQuery.isEmpty() || maximumDistance < 0 || maximumResults <= 0)
        return matches;

    for (const QString &candidate : candidates) {
        if (candidate == normalizedQuery)
            continue;
        const int distance = editDistance(normalizedQuery, candidate, maximumDistance);
        if (distance <= maximumDistance)
            matches.append(FuzzyMatch{candidate, distance});
    }
    std::sort(matches.begin(), matches.end(), [](const FuzzyMatch &left, const FuzzyMatch &right) {
        if (left.distance != right.distance)
            return left.distance < right.distance;
        return left.candidate.compare(right.candidate, Qt::CaseInsensitive) < 0;
    });
    if (matches.size() > maximumResults)
        matches.resize(maximumResults);
    return matches;
}

int fuzzyMatchScore(const QString &query, const QString &candidate)
{
    const QString q = query.trimmed();
    if (q.size() < 2 || candidate.isEmpty())
        return -1;

    const int maximumDistance = q.size() <= 4 ? 1 : (q.size() <= 8 ? 2 : 3);
    int bestScore = -1;
    const int lengthDifference = qAbs(q.size() - candidate.size());
    if (lengthDifference <= maximumDistance && candidate.size() <= q.size() * 2 + 4) {
        const int distance = editDistance(q, candidate, maximumDistance);
        if (distance > 0 && distance <= maximumDistance)
            bestScore = 1000 + distance * 70 + lengthDifference * 5;
    }

    const int sequenceScore = subsequenceScore(q, candidate);
    if (sequenceScore >= 0 && (bestScore < 0 || sequenceScore < bestScore))
        bestScore = sequenceScore;
    return bestScore;
}
}
