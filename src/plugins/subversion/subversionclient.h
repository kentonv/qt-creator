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

#ifndef SUBVERSIONCLIENT_H
#define SUBVERSIONCLIENT_H

#include "subversionsettings.h"
#include <vcsbase/vcsbaseclient.h>
#include <vcsbase/vcscommand.h>

#include <utils/fileutils.h>

namespace Subversion {
namespace Internal {

class SubversionSettings;

class SubversionClient : public VcsBase::VcsBaseClient
{
    Q_OBJECT

public:
    SubversionClient(SubversionSettings *settings);

    SubversionSettings *settings() const;

    VcsBase::VcsCommand *createCommitCmd(const QString &repositoryRoot,
                                         const QStringList &files,
                                         const QString &commitMessageFile,
                                         const QStringList &extraOptions = QStringList()) const;
    void commit(const QString &repositoryRoot,
                const QStringList &files,
                const QString &commitMessageFile,
                const QStringList &extraOptions = QStringList());

    void diff(const QString &workingDir, const QStringList &files,
              const QStringList &extraOptions = QStringList());
    QString findTopLevelForFile(const QFileInfo &file) const;
    QStringList revisionSpec(const QString &revision) const;
    StatusItem parseStatusLine(const QString &line) const;

    class Version {
    public:
        int majorVersion;
        int minorVersion;
        int patchVersion;
    };

    Version svnVersion();

    // Add authorization options to the command line arguments.
    QStringList authenticationOptions(VcsCommandTag cmd) const;
    static QStringList addAuthenticationOptions(const QStringList &args,
                                                const QString &userName = QString(),
                                                const QString &password = QString());

protected:
    Core::Id vcsEditorKind(VcsCommandTag cmd) const;
    VcsBase::VcsBaseEditorParameterWidget *createDiffEditor(const QString &workingDir,
                                                            const QStringList &files,
                                                            const QStringList &extraOptions);
private:
    Utils::FileName m_svnVersionBinary;
    QString m_svnVersion;
};

} // namespace Internal
} // namespace Subversion

#endif // SUBVERSIONCLIENT_H
