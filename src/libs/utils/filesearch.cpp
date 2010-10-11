/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2010 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/

#include "filesearch.h"
#include <cctype>

#include <QtCore/QIODevice>
#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QFutureInterface>
#include <QtCore/QtConcurrentRun>
#include <QtCore/QRegExp>
#include <QtCore/QCoreApplication>

#include <qtconcurrent/runextensions.h>

using namespace Utils;

static inline QString msgCanceled(const QString &searchTerm, int numMatches, int numFilesSearched)
{
    return QCoreApplication::translate("Utils::FileSearch",
                                       "%1: canceled. %n occurrences found in %2 files.",
                                       0, QCoreApplication::CodecForTr, numMatches).
                                       arg(searchTerm).arg(numFilesSearched);
}

static inline QString msgFound(const QString &searchTerm, int numMatches, int numFilesSearched)
{
    return QCoreApplication::translate("Utils::FileSearch",
                                       "%1: %n occurrences found in %2 files.",
                                       0, QCoreApplication::CodecForTr, numMatches).
                                       arg(searchTerm).arg(numFilesSearched);
}

static inline QString msgFound(const QString &searchTerm, int numMatches, int numFilesSearched, int filesSize)
{
    return QCoreApplication::translate("Utils::FileSearch",
                                       "%1: %n occurrences found in %2 of %3 files.",
                                       0, QCoreApplication::CodecForTr, numMatches).
                                       arg(searchTerm).arg(numFilesSearched).arg(filesSize);
}

namespace {

void runFileSearch(QFutureInterface<FileSearchResultList> &future,
                   QString searchTerm,
                   FileIterator *files,
                   QTextDocument::FindFlags flags,
                   QMap<QString, QString> fileToContentsMap)
{
    int numFilesSearched = 0;
    int numMatches = 0;
    future.setProgressRange(0, files->maxProgress());
    future.setProgressValueAndText(files->currentProgress(), msgFound(searchTerm, numMatches, numFilesSearched));

    bool caseInsensitive = !(flags & QTextDocument::FindCaseSensitively);
    bool wholeWord = (flags & QTextDocument::FindWholeWords);

    const QString searchTermLower = searchTerm.toLower();
    const QString searchTermUpper = searchTerm.toUpper();

    int termLength = searchTerm.length();
    int termMaxIndex = termLength - 1;
    const QChar *termData = searchTerm.constData();
    const QChar *termDataLower = searchTermLower.constData();
    const QChar *termDataUpper = searchTermUpper.constData();

    int chunkSize = qMax(100000, 2 * termLength);

    QFile file;
    QString str;
    QTextStream stream;
    FileSearchResultList results;
    while (files->hasNext()) {
        const QString &s = files->next();
        if (future.isPaused())
            future.waitForResume();
        if (future.isCanceled()) {
            future.setProgressValueAndText(files->currentProgress(), msgCanceled(searchTerm, numMatches, numFilesSearched));
            break;
        }

        bool needsToCloseFile = false;
        if (fileToContentsMap.contains(s)) {
            str = fileToContentsMap.value(s);
            stream.setString(&str);
        } else {
            file.setFileName(s);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            needsToCloseFile = true;
            stream.setDevice(&file);
            stream.setCodec(files->encoding());
        }

        int lineNr = 1;
        const QChar *startOfLastLine = NULL;
        bool firstChunk = true;
        while (!stream.atEnd()) {
            int chunkProcessingStart = 0;
            if (!firstChunk) {
                // we need one additional char to the left and right
                // for whole word searches
                // so we jump back two additional chars, and start at index 1
                stream.seek(stream.pos() - termLength - 1);
                chunkProcessingStart = 1;
            }
            firstChunk = false;

            const QString chunk = stream.read(chunkSize);
            int chunkLength = chunk.length();
            const QChar *chunkPtr = chunk.constData();
            // we need one additional char to the right for whole word searches,
            // except at the very end
            const QChar *chunkProcessingEnd = (stream.atEnd() ? chunkPtr + chunkLength : chunkPtr + chunkLength - 1);

            startOfLastLine = chunkPtr;
            for (const QChar *regionPtr = chunkPtr + chunkProcessingStart;
                    regionPtr + termMaxIndex < chunkProcessingEnd;
                    ++regionPtr) {
                const QChar *regionEnd = regionPtr + termMaxIndex;
                if (*regionPtr == QLatin1Char('\n')) {
                    startOfLastLine = regionPtr + 1;
                    ++lineNr;
                } else if ( /* optimization check for start and end of region */
                        // case sensitive
                        (!caseInsensitive && *regionPtr == termData[0] && *regionEnd == termData[termMaxIndex])
                        ||
                        // case insensitive
                        (caseInsensitive && (*regionPtr == termDataLower[0] || *regionPtr == termDataUpper[0])
                        && (*regionEnd == termDataLower[termMaxIndex] || *regionEnd == termDataUpper[termMaxIndex]))
                         ) {
                    bool equal = true;

                    // whole word check
                    const QChar *beforeRegion = regionPtr - 1;
                    const QChar *afterRegion = regionEnd + 1;
                    if (wholeWord && (
                            ((beforeRegion >= chunkPtr) && (beforeRegion->isLetterOrNumber() || ((*beforeRegion) == QLatin1Char('_')))) ||
                            ((afterRegion < chunkPtr + chunkLength) && (afterRegion->isLetterOrNumber() || ((*afterRegion) == QLatin1Char('_'))))
                            )) {
                        equal = false;
                    }

                    if (equal) {
                        // check all chars
                        int regionIndex = 1;
                        for (const QChar *regionCursor = regionPtr + 1; regionCursor < regionEnd; ++regionCursor, ++regionIndex) {
                            if (  // case sensitive
                                  (!caseInsensitive && *regionCursor != termData[regionIndex])
                                  ||
                                  // case insensitive
                                  (caseInsensitive && *regionCursor != termData[regionIndex]
                                   && *regionCursor != termDataLower[regionIndex] && *regionCursor != termDataUpper[regionIndex])
                                   ) {
                                equal = false;
                            }
                        }
                    }
                    if (equal) {
                        int textLength = chunkLength - (startOfLastLine - chunkPtr);
                        if (textLength > 0) {
                            QString res;
                            res.reserve(256);
                            int i = 0;
                            int n = 0;
                            while (startOfLastLine[i] != QLatin1Char('\n') && startOfLastLine[i] != QLatin1Char('\r') && i < textLength && n++ < 256)
                                res.append(startOfLastLine[i++]);
                            res.squeeze();
                            results << FileSearchResult(s, lineNr, res,
                                                          regionPtr - startOfLastLine, termLength,
                                                          QStringList());
                            ++numMatches;
                        }
                    }
                }
            }
        }

        ++numFilesSearched;
        if (future.isProgressUpdateNeeded()) {
            if (!results.isEmpty()) {
                future.reportResult(results);
                results.clear();
            }
            future.setProgressRange(0, files->maxProgress());
            future.setProgressValueAndText(files->currentProgress(), msgFound(searchTerm, numMatches, numFilesSearched));
        }

        // clean up
        if (needsToCloseFile)
            file.close();

    }
    if (!results.isEmpty()) {
        future.reportResult(results);
        results.clear();
    }
    if (!future.isCanceled())
        future.setProgressValueAndText(files->currentProgress(), msgFound(searchTerm, numMatches, numFilesSearched));
    delete files;
}

void runFileSearchRegExp(QFutureInterface<FileSearchResultList> &future,
                   QString searchTerm,
                   FileIterator *files,
                   QTextDocument::FindFlags flags,
                   QMap<QString, QString> fileToContentsMap)
{
    int numFilesSearched = 0;
    int numMatches = 0;
    future.setProgressRange(0, files->maxProgress());
    future.setProgressValueAndText(files->currentProgress(), msgFound(searchTerm, numMatches, numFilesSearched));
    if (flags & QTextDocument::FindWholeWords)
        searchTerm = QString::fromLatin1("\\b%1\\b").arg(searchTerm);
    const Qt::CaseSensitivity caseSensitivity = (flags & QTextDocument::FindCaseSensitively) ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const QRegExp expression(searchTerm, caseSensitivity);

    QFile file;
    QString str;
    QTextStream stream;
    FileSearchResultList results;
    while (files->hasNext()) {
        const QString &s = files->next();
        if (future.isPaused())
            future.waitForResume();
        if (future.isCanceled()) {
            future.setProgressValueAndText(files->currentProgress(), msgCanceled(searchTerm, numMatches, numFilesSearched));
            break;
        }

        bool needsToCloseFile = false;
        if (fileToContentsMap.contains(s)) {
            str = fileToContentsMap.value(s);
            stream.setString(&str);
        } else {
            file.setFileName(s);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            needsToCloseFile = true;
            stream.setDevice(&file);
            stream.setCodec(files->encoding());
        }
        int lineNr = 1;
        QString line;
        while (!stream.atEnd()) {
            line = stream.readLine();
            int pos = 0;
            while ((pos = expression.indexIn(line, pos)) != -1) {
                results << FileSearchResult(s, lineNr, line,
                                              pos, expression.matchedLength(),
                                              expression.capturedTexts());
                ++numMatches;
                pos += expression.matchedLength();
            }
            ++lineNr;
        }
        ++numFilesSearched;
        if (future.isProgressUpdateNeeded()) {
            if (!results.isEmpty()) {
                future.reportResult(results);
                results.clear();
            }
            future.setProgressRange(0, files->maxProgress());
            future.setProgressValueAndText(files->currentProgress(), msgFound(searchTerm, numMatches, numFilesSearched));
        }
        if (needsToCloseFile)
            file.close();
    }
    if (!results.isEmpty()) {
        future.reportResult(results);
        results.clear();
    }
    if (!future.isCanceled())
        future.setProgressValueAndText(files->currentProgress(), msgFound(searchTerm, numMatches, numFilesSearched));
    delete files;
}

} // namespace


QFuture<FileSearchResultList> Utils::findInFiles(const QString &searchTerm, FileIterator *files,
    QTextDocument::FindFlags flags, QMap<QString, QString> fileToContentsMap)
{
    return QtConcurrent::run<FileSearchResultList, QString, FileIterator *, QTextDocument::FindFlags, QMap<QString, QString> >
            (runFileSearch, searchTerm, files, flags, fileToContentsMap);
}

QFuture<FileSearchResultList> Utils::findInFilesRegExp(const QString &searchTerm, FileIterator *files,
    QTextDocument::FindFlags flags, QMap<QString, QString> fileToContentsMap)
{
    return QtConcurrent::run<FileSearchResultList, QString, FileIterator *, QTextDocument::FindFlags, QMap<QString, QString> >
            (runFileSearchRegExp, searchTerm, files, flags, fileToContentsMap);
}

QString Utils::expandRegExpReplacement(const QString &replaceText, const QStringList &capturedTexts)
{
    // handles \1 \\ \& & \t
    QString result;
    int numCaptures = capturedTexts.size() - 1;
    for (int i = 0; i < replaceText.length(); ++i) {
        QChar c = replaceText.at(i);
        if (c == QLatin1Char('\\') && i < replaceText.length() - 1) {
            c = replaceText.at(++i);
            if (c == QLatin1Char('\\')) {
                result += QLatin1Char('\\');
            } else if (c == QLatin1Char('&')) {
                result += QLatin1Char('&');
            } else if (c == QLatin1Char('t')) {
                result += QLatin1Char('\t');
            } else if (c.isDigit()) {
                int index = c.unicode()-'1';
                if (index < numCaptures) {
                    result += capturedTexts.at(index+1);
                } else {
                    result += QLatin1Char('\\');
                    result += c;
                }
            } else {
                result += QLatin1Char('\\');
                result += c;
            }
        } else if (c == QLatin1Char('&')) {
            result += capturedTexts.at(0);
        } else {
            result += c;
        }
    }
    return result;
}

// #pragma mark -- FileIterator

FileIterator::FileIterator()
    : m_list(QStringList()),
    m_iterator(0),
    m_index(-1)
{
}

FileIterator::FileIterator(const QStringList &fileList,
                           const QList<QTextCodec *> encodings)
    : m_list(fileList),
      m_iterator(new QStringListIterator(m_list)),
      m_encodings(encodings),
      m_index(-1)
{
}

FileIterator::~FileIterator()
{
    if (m_iterator)
        delete m_iterator;
}

bool FileIterator::hasNext() const
{
    Q_ASSERT(m_iterator);
    return m_iterator->hasNext();
}

QString FileIterator::next()
{
    Q_ASSERT(m_iterator);
    ++m_index;
    return m_iterator->next();
}

int FileIterator::maxProgress() const
{
    return m_list.size();
}

int FileIterator::currentProgress() const
{
    return m_index + 1;
}

QTextCodec * FileIterator::encoding() const
{
    if (m_index >= 0 && m_index < m_encodings.size())
        return m_encodings.at(m_index);
    return QTextCodec::codecForLocale();
}

// #pragma mark -- SubDirFileIterator

namespace {
    const int MAX_PROGRESS = 1000;
}

SubDirFileIterator::SubDirFileIterator(const QStringList &directories, const QStringList &filters,
                                       QTextCodec *encoding)
    : m_filters(filters), m_progress(0)
{
    m_encoding = (encoding == 0 ? QTextCodec::codecForLocale() : encoding);
    qreal maxPer = MAX_PROGRESS/directories.count();
    foreach (const QString &directoryEntry, directories) {
        if (!directoryEntry.isEmpty()) {
            m_dirs.push(QDir(directoryEntry));
            m_progressValues.push(maxPer);
            m_processedValues.push(false);
        }
    }
}

bool SubDirFileIterator::hasNext() const
{
    if (!m_currentFiles.isEmpty())
        return true;
    while(!m_dirs.isEmpty() && m_currentFiles.isEmpty()) {
        QDir dir = m_dirs.pop();
        qreal dirProgressMax = m_progressValues.pop();
        bool processed = m_processedValues.pop();
        if (dir.exists()) {
            QStringList subDirs;
            if (!processed) {
                subDirs = dir.entryList(QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot);
            }
            if (subDirs.isEmpty()) {
                QStringList fileEntries = dir.entryList(m_filters,
                    QDir::Files|QDir::Hidden);
                QStringListIterator it(fileEntries);
                it.toBack();
                while (it.hasPrevious()) {
                    const QString &file = it.previous();
                    m_currentFiles.append(dir.path()+ QLatin1Char('/') +file);
                }
                m_progress += dirProgressMax;
            } else {
                qreal subProgress = dirProgressMax/(subDirs.size()+1);
                m_dirs.push(dir);
                m_progressValues.push(subProgress);
                m_processedValues.push(true);
                QStringListIterator it(subDirs);
                it.toBack();
                while (it.hasPrevious()) {
                    const QString &directory = it.previous();
                    m_dirs.push(QDir(dir.path()+ QLatin1Char('/') + directory));
                    m_progressValues.push(subProgress);
                    m_processedValues.push(false);
                }
            }
        } else {
            m_progress += dirProgressMax;
        }
    }
    if (m_currentFiles.isEmpty()) {
        m_progress = MAX_PROGRESS;
        return false;
    }

    return true;
}

QString SubDirFileIterator::next()
{
    Q_ASSERT(!m_currentFiles.isEmpty());
    return m_currentFiles.takeFirst();
}

int SubDirFileIterator::maxProgress() const
{
    return MAX_PROGRESS;
}

int SubDirFileIterator::currentProgress() const
{
    return qMin(qRound(m_progress), MAX_PROGRESS);
}

QTextCodec * SubDirFileIterator::encoding() const
{
    return m_encoding;
}
