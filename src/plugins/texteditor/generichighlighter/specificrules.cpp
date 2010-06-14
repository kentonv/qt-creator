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

#include "specificrules.h"
#include "highlightdefinition.h"
#include "keywordlist.h"
#include "progressdata.h"
#include "reuse.h"

#include <QLatin1Char>

using namespace TextEditor;
using namespace Internal;

namespace {

void replaceByCaptures(QChar *c, const QStringList &captures)
{
    int index = c->digitValue();
    if (index > 0) {
        const QString &capture = captures.at(index);
        if (!capture.isEmpty())
            *c = capture.at(0);
    }
}

void replaceByCaptures(QString *s, const QStringList &captures)
{
    static const QLatin1Char kPercent('%');

    int index;
    int from = 0;
    while ((index = s->indexOf(kPercent, from)) != -1) {
        from = index + 1;

        QString accumulator;
        while (from < s->length() && s->at(from).isDigit()) {
            accumulator.append(s->at(from));
            ++from;
        }

        bool ok;
        int number = accumulator.toInt(&ok);
        Q_ASSERT(ok);

        s->replace(index, accumulator.length() + 1, captures.at(number));
        index = from;
    }
}
}

// DetectChar
void DetectCharRule::setChar(const QString &character)
{ setStartCharacter(&m_char, character); }

void DetectCharRule::doReplaceExpressions(const QStringList &captures)
{ replaceByCaptures(&m_char, captures); }

bool DetectCharRule::doMatchSucceed(const QString &text,
                                    const int length,
                                    ProgressData *progress) const
{
    return matchCharacter(text, length, progress, m_char);
}

// Detect2Chars
void Detect2CharsRule::setChar(const QString &character)
{ setStartCharacter(&m_char, character); }

void Detect2CharsRule::setChar1(const QString &character)
{ setStartCharacter(&m_char1, character); }

void Detect2CharsRule::doReplaceExpressions(const QStringList &captures)
{
    replaceByCaptures(&m_char, captures);
    replaceByCaptures(&m_char1, captures);
}

bool Detect2CharsRule::doMatchSucceed(const QString &text,
                                      const int length,
                                      ProgressData *progress) const
{
    if (matchCharacter(text, length, progress, m_char)) {
        if (progress->offset() < length && matchCharacter(text, length, progress, m_char1, false))
            return true;
        else
            progress->restoreOffset();
    }

    return false;
}

// AnyChar
void AnyCharRule::setCharacterSet(const QString &s)
{ m_characterSet = s; }

bool AnyCharRule::doMatchSucceed(const QString &text,
                                 const int length,
                                 ProgressData *progress) const
{
    Q_UNUSED(length)

    if (m_characterSet.contains(text.at(progress->offset()))) {
        progress->incrementOffset();
        return true;
    }

    return false;
}

// StringDetect
void StringDetectRule::setString(const QString &s)
{
    m_string = s;
    m_length = m_string.length();
}

void StringDetectRule::setInsensitive(const QString &insensitive)
{ m_caseSensitivity = toCaseSensitivity(!toBool(insensitive)); }

void StringDetectRule::doReplaceExpressions(const QStringList &captures)
{
    replaceByCaptures(&m_string, captures);
    m_length = m_string.length();
}

bool StringDetectRule::doMatchSucceed(const QString &text,
                                      const int length,
                                      ProgressData *progress) const
{
    if (length - progress->offset() >= m_length) {
        QString candidate = text.fromRawData(text.unicode() + progress->offset(), m_length);
        if (candidate.compare(m_string, m_caseSensitivity) == 0) {
            progress->incrementOffset(m_length);
            return true;
        }
    }

    return false;
}

// RegExpr
void RegExprRule::setPattern(const QString &pattern)
{ m_expression.setPattern(pattern); }

void RegExprRule::setInsensitive(const QString &insensitive)
{ m_expression.setCaseSensitivity(toCaseSensitivity(!toBool(insensitive))); }

void RegExprRule::setMinimal(const QString &minimal)
{ m_expression.setMinimal(toBool(minimal)); }

void RegExprRule::doReplaceExpressions(const QStringList &captures)
{
    QString s = m_expression.pattern();
    replaceByCaptures(&s, captures);
    m_expression.setPattern(s);
}

bool RegExprRule::doMatchSucceed(const QString &text,
                                 const int length,
                                 ProgressData *progress) const
{
    Q_UNUSED(length)

    // This is not documented but a regular expression match is considered valid if it starts
    // at the current position and if the match length is not zero.
    const int offset = progress->offset();
    if (m_expression.indexIn(text, offset, QRegExp::CaretAtZero) == offset) {
        if (m_expression.matchedLength() == 0)
            return false;
        progress->incrementOffset(m_expression.matchedLength());
        progress->setCaptures(m_expression.capturedTexts());
        return true;
    }

    return false;
}

// Keyword
KeywordRule::KeywordRule(const QSharedPointer<HighlightDefinition> &definition) :
    m_overrideGlobal(false)
{
    setDefinition(definition);
}

KeywordRule::~KeywordRule()
{}

void KeywordRule::setInsensitive(const QString &insensitive)
{
    if (!insensitive.isEmpty()) {
        m_overrideGlobal = true;
        m_localCaseSensitivity = toCaseSensitivity(!toBool(insensitive));
    }
}

void KeywordRule::setList(const QString &listName)
{ m_list = definition()->keywordList(listName); }

bool KeywordRule::doMatchSucceed(const QString &text,
                                 const int length,
                                 ProgressData *progress) const
{
    int current = progress->offset();

    if (current > 0 && !definition()->isDelimiter(text.at(current - 1)))
        return false;
    if (definition()->isDelimiter(text.at(current)))
        return false;

    while (current < length && !definition()->isDelimiter(text.at(current)))
        ++current;

    QString candidate =
        QString::fromRawData(text.unicode() + progress->offset(), current - progress->offset());
    if ((m_overrideGlobal && m_list->isKeyword(candidate, m_localCaseSensitivity)) ||
        (!m_overrideGlobal && m_list->isKeyword(candidate, definition()->keywordsSensitive()))) {
        progress->setOffset(current);
        return true;
    }

    return false;
}

// Int
bool IntRule::doMatchSucceed(const QString &text,
                             const int length,
                             ProgressData *progress) const
{
    const int offset = progress->offset();

    // This is necessary to correctly highlight an invalid octal like 09, for example.
    if (offset > 0 && text.at(offset - 1).isDigit())
        return false;

    if (text.at(offset).isDigit() && text.at(offset) != kZero) {
        progress->incrementOffset();
        charPredicateMatchSucceed(text, length, progress, &QChar::isDigit);
        return true;
    }

    return false;
}

// Float
bool FloatRule::doMatchSucceed(const QString &text, const int length, ProgressData *progress) const
{
    progress->saveOffset();

    bool integralPart = charPredicateMatchSucceed(text, length, progress, &QChar::isDigit);

    bool decimalPoint = false;
    if (progress->offset() < length && text.at(progress->offset()) == kDot) {
        progress->incrementOffset();
        decimalPoint = true;
    }

    bool fractionalPart = charPredicateMatchSucceed(text, length, progress, &QChar::isDigit);

    bool exponentialPart = false;
    int offset = progress->offset();
    if (offset < length && (text.at(offset) == kE || text.at(offset).toLower() == kE)) {
        progress->incrementOffset();

        offset = progress->offset();
        if (offset < length && (text.at(offset) == kPlus || text.at(offset) == kMinus))
            progress->incrementOffset();

        if (charPredicateMatchSucceed(text, length, progress, &QChar::isDigit)) {
            exponentialPart = true;
        } else {
            progress->restoreOffset();
            return false;
        }
    }

    if ((integralPart || fractionalPart) && (decimalPoint || exponentialPart))
        return true;

    progress->restoreOffset();
    return false;
}

// COctal
bool HlCOctRule::doMatchSucceed(const QString &text,
                                const int length,
                                ProgressData *progress) const
{
    if (matchCharacter(text, length, progress, kZero)) {
        // In the definition files the number matching rules which are more restrictive should
        // appear before the rules which are least resctritive. Although this happens in general
        // there is at least one case where this is not strictly followed for existent definition
        // files (specifically, HlCHex comes before HlCOct). So the condition below.
        const int offset = progress->offset();
        if (offset < length && (text.at(offset) == kX || text.at(offset).toLower() == kX)) {
            progress->restoreOffset();
            return false;
        }

        charPredicateMatchSucceed(text, length, progress, &isOctalDigit);
        return true;
    }

    return false;
}

// CHex
bool HlCHexRule::doMatchSucceed(const QString &text,
                                const int length,
                                ProgressData *progress) const
{
    if (matchCharacter(text, length, progress, kZero)) {
        const int offset = progress->offset();
        if (offset < length && text.at(offset) != kX && text.at(offset).toLower() != kX) {
            progress->restoreOffset();
            return false;
        }

        progress->incrementOffset();
        if (charPredicateMatchSucceed(text, length, progress, &isHexDigit))
            return true;
        else
            progress->restoreOffset();
    }

    return false;
}

// CString
bool HlCStringCharRule::doMatchSucceed(const QString &text,
                                       const int length,
                                       ProgressData *progress) const
{
    if (matchEscapeSequence(text, length, progress))
        return true;

    if (matchOctalSequence(text, length, progress))
        return true;

    if (matchHexSequence(text, length, progress))
        return true;

    return false;
}

// CChar
bool HlCCharRule::doMatchSucceed(const QString &text,
                                 const int length,
                                 ProgressData *progress) const
{
    if (matchCharacter(text, length, progress, kSingleQuote)) {
        if (progress->offset() < length) {
            if (text.at(progress->offset()) != kBackSlash &&
                text.at(progress->offset()) != kSingleQuote) {
                progress->incrementOffset();
            } else if (!matchEscapeSequence(text, length, progress, false)) {
                progress->restoreOffset();
                return false;
            }

            if (progress->offset() < length &&
                matchCharacter(text, length, progress, kSingleQuote, false)) {
                return true;
            } else {
                progress->restoreOffset();
            }
        } else {
            progress->restoreOffset();
        }
    }

    return false;
}

// RangeDetect
void RangeDetectRule::setChar(const QString &character)
{ setStartCharacter(&m_char, character); }

void RangeDetectRule::setChar1(const QString &character)
{ setStartCharacter(&m_char1, character); }

bool RangeDetectRule::doMatchSucceed(const QString &text,
                                     const int length,
                                     ProgressData *progress) const
{
    if (matchCharacter(text, length, progress, m_char)) {
        while (progress->offset() < length) {
            if (matchCharacter(text, length, progress, m_char1, false))
                return true;
            progress->incrementOffset();
        }
        progress->restoreOffset();
    }

    return false;
}

// LineContinue
bool LineContinueRule::doMatchSucceed(const QString &text,
                                      const int length,
                                      ProgressData *progress) const
{
    if (progress->offset() != length - 1)
        return false;

    if (text.at(progress->offset()) == kBackSlash) {
        progress->incrementOffset();
        progress->setWillContinueLine(true);
        return true;
    }

    return false;
}

// DetectSpaces
DetectSpacesRule::DetectSpacesRule() : Rule(false)
{}

bool DetectSpacesRule::doMatchSucceed(const QString &text,
                                      const int length,
                                      ProgressData *progress) const
{
    return charPredicateMatchSucceed(text, length, progress, &QChar::isSpace);
}

// DetectIdentifier
bool DetectIdentifierRule::doMatchSucceed(const QString &text,
                                          const int length,
                                          ProgressData *progress) const
{
    // Identifiers are characterized by a letter or underscore as the first character and then
    // zero or more word characters (\w*).
    if (text.at(progress->offset()).isLetter() || text.at(progress->offset()) == kUnderscore) {
        progress->incrementOffset();
        while (progress->offset() < length) {
            const QChar &current = text.at(progress->offset());
            if (current.isLetterOrNumber() || current.isMark() || current == kUnderscore)
                progress->incrementOffset();
            else
                break;
        }
        return true;
    }
    return false;
}
