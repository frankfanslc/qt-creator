/****************************************************************************
**
** Copyright (C) 2018 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "sshprocess.h"

#include "sshsettings.h"

#include <utils/environment.h>

namespace QSsh {

SshProcess::SshProcess(Utils::ProcessMode processMode)
{
    setProcessMode(processMode);

    Utils::Environment env = Utils::Environment::systemEnvironment();
    if (SshSettings::askpassFilePath().exists()) {
        env.set("SSH_ASKPASS", SshSettings::askpassFilePath().toUserOutput());

        // OpenSSH only uses the askpass program if DISPLAY is set, regardless of the platform.
        if (!env.hasKey("DISPLAY"))
            env.set("DISPLAY", ":0");
    }
    setEnvironment(env);

    // Otherwise, ssh will ignore SSH_ASKPASS and read from /dev/tty directly.
    setDisableUnixTerminal();
}

} // namespace QSsh
