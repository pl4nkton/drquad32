/**
 * Copyright (C)2015 Thomas Kindler <mail_drquad@t-kindler.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "BootProtocol.h"
#include "../../Bootloader/msg_structs.h"
#include "../../Libraries/crc/crc32.h"
#include "IntelHexFile.h"
#include "MainWindow.h"

#include <QDebug>
#include <QApplication>
#include <QThread>
#include <QTime>


enum FLASH_Status {
    FLASH_BUSY = 1,
    FLASH_ERROR_RD,
    FLASH_ERROR_PGS,
    FLASH_ERROR_PGP,
    FLASH_ERROR_PGA,
    FLASH_ERROR_WRP,
    FLASH_ERROR_PROGRAM,
    FLASH_ERROR_OPERATION,
    FLASH_COMPLETE
};


static QString flashStatusStr(uint8_t status)
{
    switch (status) {
    case FLASH_BUSY             : return "FLASH_BUSY";
    case FLASH_ERROR_RD         : return "FLASH_ERROR_RD";
    case FLASH_ERROR_PGS        : return "FLASH_ERROR_PGS";
    case FLASH_ERROR_PGP        : return "FLASH_ERROR_PGP";
    case FLASH_ERROR_PGA        : return "FLASH_ERROR_PGA";
    case FLASH_ERROR_WRP        : return "FLASH_ERROR_WRP";
    case FLASH_ERROR_PROGRAM    : return "FLASH_ERROR_PROGRAM";
    case FLASH_ERROR_OPERATION  : return "FLASH_ERROR_OPERATION";
    case FLASH_COMPLETE         : return "FLASH_COMPLETE";
    default: return QString().sprintf("%d", status);
    }
}


BootProtocol::BootProtocol(QWidget *parent, Connection &connection)
    : QWidget(parent)
    , progressDialog(this)
    , connection(connection)
{
    progressDialog.setWindowModality(Qt::WindowModal);
    progressDialog.setWindowTitle("Updating...");

    connect(&connection, &Connection::messageReceived, this, &BootProtocol::connection_messageReceived);
}

BootProtocol::~BootProtocol()
{
}

void BootProtocol::connection_messageReceived(const msg_generic &msg)
{
    messageQueue.enqueue(msg);
}

void BootProtocol::showProgress(int value, const QString &text)
{
    // qDebug("%2d: %s", value, qPrintable(text));
    progressDialog.setValue(value);
    progressDialog.setLabelText(text);
}


/********** Low level functions **********/

bool BootProtocol::bootGetResponse(msg_boot_response *response, int timeout)
{
    while (timeout > 0) {
        while (!messageQueue.isEmpty()) {
            auto msg = messageQueue.dequeue();
            if (msg.h.id == MSG_ID_BOOT_RESPONSE) {
                // qDebug() << "BOOT_RESPONSE: " << msg.data;

                *response = *(msg_boot_response*)&msg;
                return true;
            }
        }

        QApplication::processEvents();
        QThread::msleep(10);
        timeout -= 10;
    }

    m_errorString = "Time out";
    return false;
}


bool BootProtocol::bootResetHack()
{
    // Hack: send ctrl-c + reset
    //
    const char *s = "\03\nreset\n";

    msg_shell_from_pc msg;
    msg.h.id = MSG_ID_SHELL_FROM_PC;
    msg.h.data_len = strlen(s);
    strcpy((char*)msg.data, s);

    connection.sendMessage(&msg.h);
    return true;
}



bool BootProtocol::bootEnter()
{
    bootResetHack();

    msg_boot_enter msg;
    msg.h.id = MSG_ID_BOOT_ENTER;
    msg.h.data_len = 4;
    msg.magic = 0xB00710AD;

    connection.sendMessage(&msg.h);

    msg_boot_response res;
    if (!bootGetResponse(&res))
        return false;

    if (res.data[0] != 1) {
        m_errorString = QString().sprintf(
            "Can't enter bootloader: %d", res.data[0]
        );
        return false;
    }

    return true;
}


bool BootProtocol::bootExit()
{
    msg_boot_exit msg;
    msg.h.id = MSG_ID_BOOT_EXIT;
    msg.h.data_len = 0;
    connection.sendMessage(&msg.h);

    msg_boot_response res;
    if (!bootGetResponse(&res))
        return false;

    if (res.data[0] != 1) {
        m_errorString = QString().sprintf(
            "Can't exit bootloader: %d", res.data[0]
        );
        return false;
    }

    return true;
}


bool BootProtocol::bootEraseSector(uint sector)
{
    msg_boot_erase_sector msg;
    msg.h.id = MSG_ID_BOOT_ERASE_SECTOR;
    msg.h.data_len = 4;
    msg.sector = sector;
    connection.sendMessage(&msg.h);

    msg_boot_response res;
    if (!bootGetResponse(&res, 2000))
        return false;

    if (res.data[0] != FLASH_COMPLETE) {
        m_errorString = QString().sprintf(
            "Can't erase sector %d: %s", sector,
            qPrintable(flashStatusStr(res.data[0]))
        );
        return false;
    }

    return true;
}


bool BootProtocol::bootWriteDataAsync(uint addr, const QByteArray &data)
{
    assert((uint)data.length() <= sizeof(msg_boot_write_data::data));

    msg_boot_write_data msg;
    msg.h.id = MSG_ID_BOOT_WRITE_DATA;
    msg.h.data_len = 4 + data.length();
    msg.address = addr;

    memcpy(msg.data, data.constData(), data.length());

    connection.sendMessage(&msg.h);

    return true;
}


bool BootProtocol::bootWriteData(uint addr, const QByteArray &data)
{
    static const int chunk_size = sizeof(msg_boot_write_data::data);

    int chunks = (data.length() + chunk_size - 1) / chunk_size;
    int ack_window = std::min(chunks, 10);
    int offset = 0;

    for (int i=0; i < chunks + ack_window; i++) {
        // send data
        //
        if (i < chunks) {
            showProgress(
                offset * 100 / data.length(),
                QString().sprintf("Writing 0x%08x", addr + offset)
            );

            bootWriteDataAsync(addr + offset, data.mid(offset, chunk_size) );
            offset += chunk_size;

            if (progressDialog.wasCanceled())
                return true;
        }

        // check responses later to mask the connection latency
        //
        if (i >= ack_window) {
            msg_boot_response res;

            if (!bootGetResponse(&res))
                return false;

            if (res.data[0] != FLASH_COMPLETE) {
                m_errorString = QString().sprintf(
                    "Can't write data at 0x%08x: %s", addr,
                    qPrintable(flashStatusStr(res.data[0]))
                );
                return false;
            }
        }
    }

    return true;
}

bool BootProtocol::bootVerifyData(uint addr, const QByteArray &data)
{
    msg_boot_verify msg;
    msg.h.id = MSG_ID_BOOT_VERIFY;
    msg.h.data_len = 4 + 4;
    msg.address = addr;
    msg.length  = data.length();

    connection.sendMessage(&msg.h);

    msg_boot_response res;
    if (!bootGetResponse(&res))
        return false;

    auto remote_crc = *(uint*)res.data;
    auto local_crc = crc32_init();
    local_crc = crc32_update(local_crc, (const uchar*)data.constData(), data.length());
    local_crc = crc32_finalize(local_crc);

    if  (remote_crc != local_crc) {
        m_errorString = QString().sprintf(
            "Image CRC check failed. Expected 0x%04x, got 0x%04x",
            local_crc, remote_crc
        );
        return false;
    }

    return true;

}


/********** Firmware update functions **********/

#define STEP(function)  do {            \
    if (!function)                      \
        return false;                   \
    if (progressDialog.wasCanceled())   \
        return true;                    \
} while(0);


bool BootProtocol::sendHexFile(const QString &fileName)
{
    QTime t0, t_enter, t_erase, t_write, t_verify, t_total;

    progressDialog.reset();
    progressDialog.show();

    showProgress(0, "Loading " + fileName);

    IntelHexFile ih;
    if (!ih.loadHex(fileName)) {
        m_errorString = ih.errorString();
        return false;
    }

    uint start_addr  = ih.sections[0].offset;
    uint end_addr    = ih.sections[0].offset +  ih.sections[0].data.size();
    const auto &data = ih.sections[0].data;

    t0 = QTime::currentTime();

    qDebug("Start 0x%08x", start_addr);
    qDebug("End   0x%08x", end_addr);

    int tries=0, ok=0;
    while (tries++ < 100 && !ok) {
        showProgress(tries, "Entering bootloader");
        STEP( (ok=bootEnter(), true) );
    }

    if (!ok) {
        m_errorString = "Can't enter boot loader";
        return false;
    }

    t_enter = QTime::currentTime();

    for (int i=4; i<12; i++) {
        showProgress(
            10 + 10 * (i-4) / (12-4),
            QString().sprintf("Erasing sector %d...", i)
        );
        STEP( bootEraseSector(i) );
    }

    t_erase = QTime::currentTime();

    // skip initial 8 bytes (written after verify)
    //
    STEP( bootWriteData(start_addr + 8, data.mid(8)) );

    t_write = QTime::currentTime();

    showProgress(85, "Verifying");
    STEP( bootVerifyData(start_addr + 8, data.mid(8)) );

    t_verify = QTime::currentTime();

    showProgress(90, "Writing first 8 bytes");
    STEP( bootWriteData(start_addr, data.mid(0, 8)) );

    showProgress(95, "Starting application");
    STEP( bootExit() );

    showProgress(100, "Done.");

    t_total =  QTime::currentTime();

    qDebug("  Enter:  %d ms", t0.msecsTo(t_enter));
    qDebug("  Erase:  %d ms", t_enter.msecsTo(t_erase));
    qDebug("  Write:  %d ms", t_erase.msecsTo(t_write));
    qDebug("  Verify: %d ms", t_write.msecsTo(t_verify));
    qDebug("  Total:  %d ms", t0.msecsTo(t_total));

    return true;
}


QString BootProtocol::errorString()
{
    return m_errorString;
}
