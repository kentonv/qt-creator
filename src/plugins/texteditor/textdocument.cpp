/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://www.qt.io/licensing.  For further information
** use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file.  Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "textdocument.h"

#include "convenience.h"
#include "extraencodingsettings.h"
#include "fontsettings.h"
#include "indenter.h"
#include "storagesettings.h"
#include "syntaxhighlighter.h"
#include "tabsettings.h"
#include "textdocumentlayout.h"
#include "texteditor.h"
#include "texteditorconstants.h"
#include "typingsettings.h"
#include <texteditor/generichighlighter/highlighter.h>

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureInterface>
#include <QScrollBar>
#include <QStringList>
#include <QTextCodec>

#include <coreplugin/coreconstants.h>
#include <coreplugin/icore.h>
#include <coreplugin/progressmanager/progressmanager.h>
#include <utils/qtcassert.h>

using namespace Core;

/*!
    \class TextEditor::BaseTextDocument
    \brief The BaseTextDocument class is the base class for QTextDocument based documents.

    It is the base class for documents used by implementations of the BaseTextEditor class,
    and contains basic functions for retrieving text content and markers (like bookmarks).

    Subclasses of BaseTextEditor can either use BaseTextDocument as is (and this is the default),
    or created subclasses of BaseTextDocument if they have special requirements.
*/

namespace TextEditor {

class TextDocumentPrivate
{
public:
    TextDocumentPrivate() :
        m_fontSettingsNeedsApply(false),
        m_highlighter(0),
        m_completionAssistProvider(0),
        m_indenter(new Indenter),
        m_fileIsReadOnly(false),
        m_autoSaveRevision(-1)
    {
    }

    QTextCursor indentOrUnindent(const QTextCursor &textCursor, bool doIndent);
    void resetRevisions();
    void updateRevisions();

public:
    QString m_defaultPath;
    QString m_suggestedFileName;
    TypingSettings m_typingSettings;
    StorageSettings m_storageSettings;
    TabSettings m_tabSettings;
    ExtraEncodingSettings m_extraEncodingSettings;
    FontSettings m_fontSettings;
    bool m_fontSettingsNeedsApply; // for applying font settings delayed till an editor becomes visible
    QTextDocument m_document;
    SyntaxHighlighter *m_highlighter;
    CompletionAssistProvider *m_completionAssistProvider;
    QScopedPointer<Indenter> m_indenter;

    bool m_fileIsReadOnly;
    int m_autoSaveRevision;

    TextMarks m_marksCache; // Marks not owned
};

QTextCursor TextDocumentPrivate::indentOrUnindent(const QTextCursor &textCursor, bool doIndent)
{
    QTextCursor cursor = textCursor;
    cursor.beginEditBlock();

    if (cursor.hasSelection()) {
        // Indent or unindent the selected lines
        int pos = cursor.position();
        int anchor = cursor.anchor();
        int start = qMin(anchor, pos);
        int end = qMax(anchor, pos);

        QTextBlock startBlock = m_document.findBlock(start);
        QTextBlock endBlock = m_document.findBlock(end-1).next();

        if (startBlock.next() == endBlock
                && (start > startBlock.position() || end < endBlock.position() - 1)) {
            // Only one line partially selected.
            cursor.removeSelectedText();
        } else {
            for (QTextBlock block = startBlock; block != endBlock; block = block.next()) {
                QString text = block.text();
                int indentPosition = m_tabSettings.lineIndentPosition(text);
                if (!doIndent && !indentPosition)
                    indentPosition = m_tabSettings.firstNonSpace(text);
                int targetColumn = m_tabSettings.indentedColumn(m_tabSettings.columnAt(text, indentPosition), doIndent);
                cursor.setPosition(block.position() + indentPosition);
                cursor.insertText(m_tabSettings.indentationString(0, targetColumn, block));
                cursor.setPosition(block.position());
                cursor.setPosition(block.position() + indentPosition, QTextCursor::KeepAnchor);
                cursor.removeSelectedText();
            }
            cursor.endEditBlock();
            return textCursor;
        }
    }

    // Indent or unindent at cursor position
    QTextBlock block = cursor.block();
    QString text = block.text();
    int indentPosition = cursor.positionInBlock();
    int spaces = m_tabSettings.spacesLeftFromPosition(text, indentPosition);
    int startColumn = m_tabSettings.columnAt(text, indentPosition - spaces);
    int targetColumn = m_tabSettings.indentedColumn(m_tabSettings.columnAt(text, indentPosition), doIndent);
    cursor.setPosition(block.position() + indentPosition);
    cursor.setPosition(block.position() + indentPosition - spaces, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.insertText(m_tabSettings.indentationString(startColumn, targetColumn, block));
    cursor.endEditBlock();
    return cursor;
}

void TextDocumentPrivate::resetRevisions()
{
    TextDocumentLayout *documentLayout = qobject_cast<TextDocumentLayout*>(m_document.documentLayout());
    QTC_ASSERT(documentLayout, return);
    documentLayout->lastSaveRevision = m_document.revision();

    for (QTextBlock block = m_document.begin(); block.isValid(); block = block.next())
        block.setRevision(documentLayout->lastSaveRevision);
}

void TextDocumentPrivate::updateRevisions()
{
    TextDocumentLayout *documentLayout = qobject_cast<TextDocumentLayout*>(m_document.documentLayout());
    QTC_ASSERT(documentLayout, return);
    int oldLastSaveRevision = documentLayout->lastSaveRevision;
    documentLayout->lastSaveRevision = m_document.revision();

    if (oldLastSaveRevision != documentLayout->lastSaveRevision) {
        for (QTextBlock block = m_document.begin(); block.isValid(); block = block.next()) {
            if (block.revision() < 0 || block.revision() != oldLastSaveRevision)
                block.setRevision(-documentLayout->lastSaveRevision - 1);
            else
                block.setRevision(documentLayout->lastSaveRevision);
        }
    }
}

///////////////////////////////////////////////////////////////////////
//
// BaseTextDocument
//
///////////////////////////////////////////////////////////////////////

TextDocument::TextDocument(Id id)
    : d(new TextDocumentPrivate)
{
    QObject::connect(&d->m_document, &QTextDocument::modificationChanged, [this](bool modified) {
        // we only want to update the block revisions when going back to the saved version,
        // e.g. with undo
        if (!modified)
            d->updateRevisions();
        emit changed();
    });

    QObject::connect(&d->m_document, &QTextDocument::contentsChanged, this, &TextDocument::contentsChanged);

    // set new document layout
    QTextOption opt = d->m_document.defaultTextOption();
    opt.setTextDirection(Qt::LeftToRight);
    opt.setFlags(opt.flags() | QTextOption::IncludeTrailingSpaces
            | QTextOption::AddSpaceForLineAndParagraphSeparators
            );
    d->m_document.setDefaultTextOption(opt);
    d->m_document.setDocumentLayout(new TextDocumentLayout(&d->m_document));

    if (id.isValid())
        setId(id);
}

TextDocument::~TextDocument()
{
    delete d;
}

QMap<QString, QString> TextDocument::openedTextDocumentContents()
{
    QMap<QString, QString> workingCopy;
    foreach (IDocument *document, DocumentModel::openedDocuments()) {
        TextDocument *textEditorDocument = qobject_cast<TextDocument *>(document);
        if (!textEditorDocument)
            continue;
        QString fileName = textEditorDocument->filePath();
        workingCopy[fileName] = textEditorDocument->plainText();
    }
    return workingCopy;
}

QMap<QString, QTextCodec *> TextDocument::openedTextDocumentEncodings()
{
    QMap<QString, QTextCodec *> workingCopy;
    foreach (IDocument *document, DocumentModel::openedDocuments()) {
        TextDocument *textEditorDocument = qobject_cast<TextDocument *>(document);
        if (!textEditorDocument)
            continue;
        QString fileName = textEditorDocument->filePath();
        workingCopy[fileName] = const_cast<QTextCodec *>(textEditorDocument->codec());
    }
    return workingCopy;
}

QString TextDocument::plainText() const
{
    return document()->toPlainText();
}

QString TextDocument::textAt(int pos, int length) const
{
    return Convenience::textAt(QTextCursor(document()), pos, length);
}

QChar TextDocument::characterAt(int pos) const
{
    return document()->characterAt(pos);
}

void TextDocument::setTypingSettings(const TypingSettings &typingSettings)
{
    d->m_typingSettings = typingSettings;
}

void TextDocument::setStorageSettings(const StorageSettings &storageSettings)
{
    d->m_storageSettings = storageSettings;
}

const TypingSettings &TextDocument::typingSettings() const
{
    return d->m_typingSettings;
}

const StorageSettings &TextDocument::storageSettings() const
{
    return d->m_storageSettings;
}

void TextDocument::setTabSettings(const TabSettings &tabSettings)
{
    if (tabSettings == d->m_tabSettings)
        return;
    d->m_tabSettings = tabSettings;

    if (Highlighter *highlighter = qobject_cast<Highlighter *>(d->m_highlighter))
        highlighter->setTabSettings(tabSettings);

    emit tabSettingsChanged();
}

const TabSettings &TextDocument::tabSettings() const
{
    return d->m_tabSettings;
}

void TextDocument::setFontSettings(const FontSettings &fontSettings)
{
    if (fontSettings == d->m_fontSettings)
        return;
    d->m_fontSettings = fontSettings;
    d->m_fontSettingsNeedsApply = true;
    emit fontSettingsChanged();
}

void TextDocument::triggerPendingUpdates()
{
    if (d->m_fontSettingsNeedsApply)
        applyFontSettings();
}

void TextDocument::setCompletionAssistProvider(CompletionAssistProvider *provider)
{
    d->m_completionAssistProvider = provider;
}

CompletionAssistProvider *TextDocument::completionAssistProvider() const
{
    return d->m_completionAssistProvider;
}

void TextDocument::applyFontSettings()
{
    d->m_fontSettingsNeedsApply = false;
    if (d->m_highlighter) {
        d->m_highlighter->setFontSettings(d->m_fontSettings);
        d->m_highlighter->rehighlight();
    }
}

const FontSettings &TextDocument::fontSettings() const
{
    return d->m_fontSettings;
}

void TextDocument::setExtraEncodingSettings(const ExtraEncodingSettings &extraEncodingSettings)
{
    d->m_extraEncodingSettings = extraEncodingSettings;
}

void TextDocument::autoIndent(const QTextCursor &cursor, QChar typedChar)
{
    d->m_indenter->indent(&d->m_document, cursor, typedChar, d->m_tabSettings);
}

void TextDocument::autoReindent(const QTextCursor &cursor)
{
    d->m_indenter->reindent(&d->m_document, cursor, d->m_tabSettings);
}

QTextCursor TextDocument::indent(const QTextCursor &cursor)
{
    return d->indentOrUnindent(cursor, true);
}

QTextCursor TextDocument::unindent(const QTextCursor &cursor)
{
    return d->indentOrUnindent(cursor, false);
}

const ExtraEncodingSettings &TextDocument::extraEncodingSettings() const
{
    return d->m_extraEncodingSettings;
}

void TextDocument::setIndenter(Indenter *indenter)
{
    // clear out existing code formatter data
    for (QTextBlock it = document()->begin(); it.isValid(); it = it.next()) {
        TextBlockUserData *userData = TextDocumentLayout::testUserData(it);
        if (userData)
            userData->setCodeFormatterData(0);
    }
    d->m_indenter.reset(indenter);
}

Indenter *TextDocument::indenter() const
{
    return d->m_indenter.data();
}

bool TextDocument::isSaveAsAllowed() const
{
    return true;
}

QString TextDocument::defaultPath() const
{
    return d->m_defaultPath;
}

QString TextDocument::suggestedFileName() const
{
    return d->m_suggestedFileName;
}

void TextDocument::setDefaultPath(const QString &defaultPath)
{
    d->m_defaultPath = defaultPath;
}

void TextDocument::setSuggestedFileName(const QString &suggestedFileName)
{
    d->m_suggestedFileName = suggestedFileName;
}

QTextDocument *TextDocument::document() const
{
    return &d->m_document;
}

SyntaxHighlighter *TextDocument::syntaxHighlighter() const
{
    return d->m_highlighter;
}

/*!
 * Saves the document to the file specified by \a fileName. If errors occur,
 * \a errorString contains their cause.
 * \a autoSave returns whether this function was called by the automatic save routine.
 * If \a autoSave is true, the cursor will be restored and some signals suppressed
 * and we do not clean up the text file (cleanWhitespace(), ensureFinalNewLine()).
 */
bool TextDocument::save(QString *errorString, const QString &saveFileName, bool autoSave)
{
    QTextCursor cursor(&d->m_document);

    // When autosaving, we don't want to modify the document/location under the user's fingers.
    TextEditorWidget *editorWidget = 0;
    int savedPosition = 0;
    int savedAnchor = 0;
    int savedVScrollBarValue = 0;
    int savedHScrollBarValue = 0;
    int undos = d->m_document.availableUndoSteps();

    // When saving the current editor, make sure to maintain the cursor and scroll bar
    // positions for undo
    if (BaseTextEditor *editor = BaseTextEditor::currentTextEditor()) {
        if (editor->document() == this) {
            editorWidget = editor->editorWidget();
            QTextCursor cur = editor->textCursor();
            savedPosition = cur.position();
            savedAnchor = cur.anchor();
            savedVScrollBarValue = editorWidget->verticalScrollBar()->value();
            savedHScrollBarValue = editorWidget->horizontalScrollBar()->value();
            cursor.setPosition(cur.position());
        }
    }

    if (!autoSave) {
        cursor.beginEditBlock();
        cursor.movePosition(QTextCursor::Start);

        if (d->m_storageSettings.m_cleanWhitespace)
          cleanWhitespace(cursor, d->m_storageSettings.m_cleanIndentation, d->m_storageSettings.m_inEntireDocument);
        if (d->m_storageSettings.m_addFinalNewLine)
          ensureFinalNewLine(cursor);
        cursor.endEditBlock();
      }

    QString fName = filePath();
    if (!saveFileName.isEmpty())
        fName = saveFileName;

    // check if UTF8-BOM has to be added or removed
    Utils::TextFileFormat saveFormat = format();
    if (saveFormat.codec->name() == "UTF-8" && supportsUtf8Bom()) {
        switch (d->m_extraEncodingSettings.m_utf8BomSetting) {
        case ExtraEncodingSettings::AlwaysAdd:
            saveFormat.hasUtf8Bom = true;
            break;
        case ExtraEncodingSettings::OnlyKeep:
            break;
        case ExtraEncodingSettings::AlwaysDelete:
            saveFormat.hasUtf8Bom = false;
            break;
        }
    }

    const bool ok = write(fName, saveFormat, d->m_document.toPlainText(), errorString);

    // restore text cursor and scroll bar positions
    if (autoSave && undos < d->m_document.availableUndoSteps()) {
        d->m_document.undo();
        if (editorWidget) {
            QTextCursor cur = editorWidget->textCursor();
            cur.setPosition(savedAnchor);
            cur.setPosition(savedPosition, QTextCursor::KeepAnchor);
            editorWidget->verticalScrollBar()->setValue(savedVScrollBarValue);
            editorWidget->horizontalScrollBar()->setValue(savedHScrollBarValue);
            editorWidget->setTextCursor(cur);
        }
    }

    if (!ok)
        return false;
    d->m_autoSaveRevision = d->m_document.revision();
    if (autoSave)
        return true;

    // inform about the new filename
    const QFileInfo fi(fName);
    d->m_document.setModified(false); // also triggers update of the block revisions
    setFilePath(QDir::cleanPath(fi.absoluteFilePath()));
    emit changed();
    return true;
}

bool TextDocument::setContents(const QByteArray &contents)
{
    return setPlainText(QString::fromUtf8(contents));
}

bool TextDocument::shouldAutoSave() const
{
    return d->m_autoSaveRevision != d->m_document.revision();
}

void TextDocument::setFilePath(const QString &newName)
{
    if (newName == filePath())
        return;
    const QFileInfo fi(newName);
    IDocument::setFilePath(QDir::cleanPath(fi.absoluteFilePath()));
}

bool TextDocument::isFileReadOnly() const
{
    if (filePath().isEmpty()) //have no corresponding file, so editing is ok
        return false;
    return d->m_fileIsReadOnly;
}

bool TextDocument::isModified() const
{
    return d->m_document.isModified();
}

void TextDocument::checkPermissions()
{
    bool previousReadOnly = d->m_fileIsReadOnly;
    if (!filePath().isEmpty()) {
        const QFileInfo fi(filePath());
        d->m_fileIsReadOnly = !fi.isWritable();
    } else {
        d->m_fileIsReadOnly = false;
    }
    if (previousReadOnly != d->m_fileIsReadOnly)
        emit changed();
}

bool TextDocument::open(QString *errorString, const QString &fileName, const QString &realFileName)
{
    QStringList content;

    ReadResult readResult = Utils::TextFileFormat::ReadIOError;

    if (!fileName.isEmpty()) {
        const QFileInfo fi(fileName);
        d->m_fileIsReadOnly = !fi.isWritable();
        readResult = read(realFileName, &content, errorString);

        d->m_document.setModified(false);
        const int chunks = content.size();
        if (chunks == 0) {
            d->m_document.clear();
        } else if (chunks == 1) {
            d->m_document.setPlainText(content.at(0));
        } else {
            QFutureInterface<void> interface;
            interface.setProgressRange(0, chunks);
            ProgressManager::addTask(interface.future(), tr("Opening File"), Constants::TASK_OPEN_FILE);
            interface.reportStarted();
            d->m_document.setUndoRedoEnabled(false);
            QTextCursor c(&d->m_document);
            c.beginEditBlock();
            d->m_document.clear();
            for (int i = 0; i < chunks; ++i) {
                c.insertText(content.at(i));
                interface.setProgressValue(i + 1);
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            }
            c.endEditBlock();
            d->m_document.setUndoRedoEnabled(true);
            interface.reportFinished();
        }
        TextDocumentLayout *documentLayout =
            qobject_cast<TextDocumentLayout*>(d->m_document.documentLayout());
        QTC_ASSERT(documentLayout, return true);
        documentLayout->lastSaveRevision = d->m_autoSaveRevision = d->m_document.revision();
        d->updateRevisions();
        d->m_document.setModified(fileName != realFileName);
        setFilePath(QDir::cleanPath(fi.absoluteFilePath()));
    }
    return readResult == Utils::TextFileFormat::ReadSuccess
           || readResult == Utils::TextFileFormat::ReadEncodingError;
}

bool TextDocument::reload(QString *errorString, QTextCodec *codec)
{
    QTC_ASSERT(codec, return false);
    setCodec(codec);
    return reload(errorString);
}

bool TextDocument::reload(QString *errorString)
{
    emit aboutToReload();
    TextDocumentLayout *documentLayout =
        qobject_cast<TextDocumentLayout*>(d->m_document.documentLayout());
    TextMarks marks;
    if (documentLayout)
        marks = documentLayout->documentClosing(); // removes text marks non-permanently

    bool success = open(errorString, filePath(), filePath());

    if (documentLayout)
        documentLayout->documentReloaded(marks, this); // re-adds text marks
    emit reloadFinished(success);
    return success;
}

bool TextDocument::setPlainText(const QString &text)
{
    if (text.size() > EditorManager::maxTextFileSize()) {
        document()->setPlainText(TextEditorWidget::msgTextTooLarge(text.size()));
        d->resetRevisions();
        document()->setModified(false);
        return false;
    }
    document()->setPlainText(text);
    d->resetRevisions();
    document()->setModified(false);
    return true;
}

bool TextDocument::reload(QString *errorString, ReloadFlag flag, ChangeType type)
{
    if (flag == FlagIgnore)
        return true;
    if (type == TypePermissions) {
        checkPermissions();
        return true;
    } else {
        return reload(errorString);
    }
}

void TextDocument::setSyntaxHighlighter(SyntaxHighlighter *highlighter)
{
    if (d->m_highlighter)
        delete d->m_highlighter;
    d->m_highlighter = highlighter;
    d->m_highlighter->setParent(this);
    d->m_highlighter->setDocument(&d->m_document);
}

void TextDocument::cleanWhitespace(const QTextCursor &cursor)
{
    bool hasSelection = cursor.hasSelection();
    QTextCursor copyCursor = cursor;
    copyCursor.setVisualNavigation(false);
    copyCursor.beginEditBlock();
    cleanWhitespace(copyCursor, true, true);
    if (!hasSelection)
        ensureFinalNewLine(copyCursor);
    copyCursor.endEditBlock();
}

void TextDocument::cleanWhitespace(QTextCursor &cursor, bool cleanIndentation, bool inEntireDocument)
{
    TextDocumentLayout *documentLayout = qobject_cast<TextDocumentLayout*>(d->m_document.documentLayout());
    Q_ASSERT(cursor.visualNavigation() == false);

    QTextBlock block = d->m_document.findBlock(cursor.selectionStart());
    QTextBlock end;
    if (cursor.hasSelection())
        end = d->m_document.findBlock(cursor.selectionEnd()-1).next();

    while (block.isValid() && block != end) {

        if (inEntireDocument || block.revision() != documentLayout->lastSaveRevision) {

            QString blockText = block.text();
            d->m_tabSettings.removeTrailingWhitespace(cursor, block);
            if (cleanIndentation && !d->m_tabSettings.isIndentationClean(block)) {
                cursor.setPosition(block.position());
                int firstNonSpace = d->m_tabSettings.firstNonSpace(blockText);
                if (firstNonSpace == blockText.length()) {
                    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                    cursor.removeSelectedText();
                } else {
                    int column = d->m_tabSettings.columnAt(blockText, firstNonSpace);
                    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, firstNonSpace);
                    QString indentationString = d->m_tabSettings.indentationString(0, column, block);
                    cursor.insertText(indentationString);
                }
            }
        }

        block = block.next();
    }
}

void TextDocument::ensureFinalNewLine(QTextCursor& cursor)
{
    cursor.movePosition(QTextCursor::End, QTextCursor::MoveAnchor);
    bool emptyFile = !cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);

    if (!emptyFile && cursor.selectedText().at(0) != QChar::ParagraphSeparator)
    {
        cursor.movePosition(QTextCursor::End, QTextCursor::MoveAnchor);
        cursor.insertText(QLatin1String("\n"));
    }
}

TextMarks TextDocument::marks() const
{
    return d->m_marksCache;
}

bool TextDocument::addMark(TextMark *mark)
{
    if (mark->baseTextDocument())
        return false;
    QTC_ASSERT(mark->lineNumber() >= 1, return false);
    int blockNumber = mark->lineNumber() - 1;
    auto documentLayout = qobject_cast<TextDocumentLayout*>(d->m_document.documentLayout());
    QTC_ASSERT(documentLayout, return false);
    QTextBlock block = d->m_document.findBlockByNumber(blockNumber);

    if (block.isValid()) {
        TextBlockUserData *userData = TextDocumentLayout::userData(block);
        userData->addMark(mark);
        d->m_marksCache.append(mark);
        mark->updateLineNumber(blockNumber + 1);
        QTC_CHECK(mark->lineNumber() == blockNumber + 1); // Checks that the base class is called
        mark->updateBlock(block);
        mark->setBaseTextDocument(this);
        if (!mark->isVisible())
            return true;
        // Update document layout
        double newMaxWidthFactor = qMax(mark->widthFactor(), documentLayout->maxMarkWidthFactor);
        bool fullUpdate =  newMaxWidthFactor > documentLayout->maxMarkWidthFactor || !documentLayout->hasMarks;
        documentLayout->hasMarks = true;
        documentLayout->maxMarkWidthFactor = newMaxWidthFactor;
        if (fullUpdate)
            documentLayout->requestUpdate();
        else
            documentLayout->requestExtraAreaUpdate();
        return true;
    }
    return false;
}

TextMarks TextDocument::marksAt(int line) const
{
    QTC_ASSERT(line >= 1, return TextMarks());
    int blockNumber = line - 1;
    QTextBlock block = d->m_document.findBlockByNumber(blockNumber);

    if (block.isValid()) {
        if (TextBlockUserData *userData = TextDocumentLayout::testUserData(block))
            return userData->marks();
    }
    return TextMarks();
}

void TextDocument::removeMarkFromMarksCache(TextMark *mark)
{
    auto documentLayout = qobject_cast<TextDocumentLayout*>(d->m_document.documentLayout());
    QTC_ASSERT(documentLayout, return);
    d->m_marksCache.removeAll(mark);

    if (d->m_marksCache.isEmpty()) {
        documentLayout->hasMarks = false;
        documentLayout->maxMarkWidthFactor = 1.0;
        documentLayout->requestUpdate();
        return;
    }

    if (!mark->isVisible())
        return;

    if (documentLayout->maxMarkWidthFactor == 1.0
            || mark->widthFactor() == 1.0
            || mark->widthFactor() < documentLayout->maxMarkWidthFactor) {
        // No change in width possible
        documentLayout->requestExtraAreaUpdate();
    } else {
        double maxWidthFactor = 1.0;
        foreach (const TextMark *mark, marks()) {
            if (!mark->isVisible())
                continue;
            maxWidthFactor = qMax(mark->widthFactor(), maxWidthFactor);
            if (maxWidthFactor == documentLayout->maxMarkWidthFactor)
                break; // Still a mark with the maxMarkWidthFactor
        }

        if (maxWidthFactor != documentLayout->maxMarkWidthFactor) {
            documentLayout->maxMarkWidthFactor = maxWidthFactor;
            documentLayout->requestUpdate();
        } else {
            documentLayout->requestExtraAreaUpdate();
        }
    }
}

void TextDocument::removeMark(TextMark *mark)
{
    QTextBlock block = d->m_document.findBlockByNumber(mark->lineNumber() - 1);
    if (TextBlockUserData *data = static_cast<TextBlockUserData *>(block.userData())) {
        if (!data->removeMark(mark))
            qDebug() << "Could not find mark" << mark << "on line" << mark->lineNumber();
    }

    removeMarkFromMarksCache(mark);
    mark->setBaseTextDocument(0);
}

void TextDocument::updateMark(TextMark *mark)
{
    Q_UNUSED(mark)
    auto documentLayout = qobject_cast<TextDocumentLayout*>(d->m_document.documentLayout());
    QTC_ASSERT(documentLayout, return);
    documentLayout->requestUpdate();
}

void TextDocument::moveMark(TextMark *mark, int previousLine)
{
    QTextBlock block = d->m_document.findBlockByNumber(previousLine - 1);
    if (TextBlockUserData *data = TextDocumentLayout::testUserData(block)) {
        if (!data->removeMark(mark))
            qDebug() << "Could not find mark" << mark << "on line" << previousLine;
    }
    removeMarkFromMarksCache(mark);
    mark->setBaseTextDocument(0);
    addMark(mark);
}

} // namespace TextEditor
