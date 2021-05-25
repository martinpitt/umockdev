/*
 * Copyright (C) 2021 Red Hat Inc.
 * Author: Benjamin Berg <bberg@redhat.com>
 *
 * umockdev is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * umockdev is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

namespace UMockdev {

using Ioctl;

private static uint8
spi_hexdigit (char c)
{
    return (c >= 'a') ? (c - 'a' + 10) :
           (c >= 'A') ? (c - 'A' + 10) :
           (c - '0');
}

private static uint8[]
spi_decode_hex (string data) throws IOError
{
    /* hex digits must come in pairs */
    if (data.length % 2 != 0)
        throw new IOError.PARTIAL_INPUT("malformed hexadecimal value: %s", data);
    var len = data.length;
    uint8[] bin = new uint8[len / 2];

    for (uint i = 0; i < bin.length; ++i)
        bin[i] = (spi_hexdigit (data[i*2]) << 4) | spi_hexdigit (data[i*2+1]);

    return bin;
}


/*
 * SPI transfers are always bi-directional. However, we have don't care
 * situations (that effectively mean discarding the written data or just
 * sending zeros). We explicitly store this and expect requests to be
 * the same at replay time.
 *
 * Also, the kernel allows CS to be kept asserted between two transfers in
 * some situations. We assume, that this works fine (i.e. there is no collision
 * of bus accesses that might cause de-assertion).
 *
 * The format of the file is uses T/C to start a transfer and W/R for the
 * write/read (tx/rx) parts. C means that it is a continuation and CS was
 * kept asserted. e.g.
 *   TW HEX
 *    R HEX
 * where HEX may not contain spaces. If W/R us "don't care", then the lines will
 * not be included. It is assumed that we never do a pure don't care transfer.
 */

private struct TransferChunk {
    uint8[]? tx;
    uint8[]? rx;
    bool cont;
}

internal abstract class IoctlSpiBase : IoctlBase {

    IoctlSpiBase(MainContext? ctx) {
        base(ctx);
    }

    internal long iter_ioctl_vector(ulong count, IoctlData data, bool for_recording) {
        long transferred = 0;

#if G_BYTE_ORDER != G_LITTLE_ENDIAN
        // This only works on little endian or 64bit.
        assert(sizeof(void*) == sizeof(uint64));
#endif

        for (long i = 0; i < count; i++) {
            unowned IoctlData tx = null, rx = null;
            Ioctl.spi_ioc_transfer *transfer = &((Ioctl.spi_ioc_transfer[]) data.data)[i];

            // Double check we don't have anything unexpected
            // We don't care about:
            //  * speed_hz
            //  * delay_usecs
            assert(transfer.bits_per_word == 0);
            assert(transfer.tx_nbits == 0);
            assert(transfer.rx_nbits == 0);
            // Requires linux headers 5.1
            //assert(transfer.word_delay_usecs == 0);

            // Resolves buffers. Mark RX buf as dirty as we will need to write it back (and don't load it).
            try {
                if (transfer.tx_buf != 0)
                    tx = data.resolve((ulong) &transfer.tx_buf - (ulong) data.data, transfer.len, true, false);
                if (transfer.rx_buf != 0)
                    rx = data.resolve((ulong) &transfer.rx_buf - (ulong) data.data, transfer.len, for_recording, !for_recording);
            } catch (IOError e) {
                warning("Error resolving IOCtl data: %s", e.message);
                return -100;
            }

            // We are good to go, now emulate the read/write or both.
            long res;
            bool keep_cs_high;
            if (i == count - 1)
                keep_cs_high = (bool) transfer.cs_change;
            else
                keep_cs_high = ! (bool) transfer.cs_change;

            res = handle_read_write(tx, rx, keep_cs_high);
            if (res < 0)
                return res;

            transferred += res;
        }

        return transferred;
    }

    internal abstract long handle_read_write(IoctlData? tx, IoctlData? rx, bool keep_cs_high);
}

internal class IoctlSpiHandler : IoctlSpiBase {

    TransferChunk[] recording;
    long replay_chunk;
    long replay_byte;

    public IoctlSpiHandler(MainContext? ctx, string file)
    {
        string val;
        base (ctx);

        // Open SPI file
        try {
            FileUtils.get_contents(file, out val);
        } catch (GLib.Error e) {
            error("Could not read %s: %s", file, e.message);
        }

        string[] lines = val.split("\n");
        unowned TransferChunk* chunk = null;
        foreach (unowned string line in lines) {
            if (line.length == 0)
                continue;

            if (line[0] == '#' || line[0] == '@')
                continue;

            if (line.length < 3)
                error("Invalid SPI transfer description");

            if (line[0] == 'C') {
                if (chunk != null)
                    chunk.cont = true;
                else
                    error("Invalid SPI transfer description, continuation without previous transfer");

                recording += TransferChunk();
                chunk = &recording[recording.length - 1];
            } else if (line[0] == 'T') {
                if (chunk != null)
                    chunk.cont = false;

                recording += TransferChunk();
                chunk = &recording[recording.length - 1];
            } else if (line[0] != ' ') {
                error("Invalid SPI transfer description");
            }

            try {
                if (line[1] == 'W') {
                    chunk.tx = spi_decode_hex(line.substring(3));
                } else if (line[1] == 'R') {
                    chunk.rx = spi_decode_hex(line.substring(3));
                } else {
                    error("Invalid transfer type, expected R or W got %c", line[1]);
                }
            } catch (IOError e) {
                error("Invalid SPI transfer description, could not decode HEX string");
            }

            if (chunk.tx != null && chunk.rx != null && chunk.tx.length != chunk.rx.length)
                error("Invalid SPI transfer description, read/write length need to be identical");
        }

        replay_chunk = 0;
        replay_byte = 0;
    }

    public override bool handle_ioctl(IoctlClient client) {
        IoctlData? data = null;
        ulong request = client.request;
        ulong size = (request >> Ioctl._IOC_SIZESHIFT) & ((1 << Ioctl._IOC_SIZEBITS) - 1);

        try {
            data = client.arg.resolve(0, size, true, true);
        } catch (IOError e) {
            warning("Error resolving IOCtl data: %s", e.message);
            return false;
        }

        if ((request & (~ (((1 << Ioctl._IOC_SIZEBITS) - 1) << Ioctl._IOC_SIZESHIFT))) == Ioctl.SPI_IOC_MESSAGE(0)) {
            assert(size % sizeof(Ioctl.spi_ioc_transfer) == 0);
            ulong count = size / sizeof(Ioctl.spi_ioc_transfer);

            long res = iter_ioctl_vector(count, data, false);

            if (res == -100)
                client.complete(-100, 0);
            else if (res < 0)
                client.complete(-1, - (int) res);
            else
                client.complete(res, 0);

            return true;
        } else {
            /* Unhandled currently:
             * Ioctl.SPI_IOC_RD_MODE
             * Ioctl.SPI_IOC_WR_MODE
             * Ioctl.SPI_IOC_RD_MODE32
             * Ioctl.SPI_IOC_WR_MODE32
             * Ioctl.SPI_IOC_RD_LSB_FIRST
             * Ioctl.SPI_IOC_WR_LSB_FIRST
             * Ioctl.SPI_IOC_RD_BITS_PER_WORD
             * Ioctl.SPI_IOC_WR_BITS_PER_WORD
             * Ioctl.SPI_IOC_RD_MAX_SPEED_HZ
             * Ioctl.SPI_IOC_WR_MAX_SPEED_HZ
             */

            client.complete(-1, Posix.ENOTTY);
            return true;
        }
    }

    public override bool handle_read(IoctlClient client) {
        long res = handle_read_write(null, client.arg, false);

        client.arg.dirty(false);
        if (res < 0)
            client.complete(-1, - (int) res);
        else
            client.complete(res, 0);

        return true;
    }

    public override bool handle_write(IoctlClient client) {
        long res = handle_read_write(client.arg, null, false);

        client.arg.dirty(false);
        if (res < 0)
            client.complete(-1, - (int) res);
        else
            client.complete(res, 0);

        return true;
    }

    internal override long handle_read_write(IoctlData? tx, IoctlData? rx, bool keep_cs_high) {
        /* We might need to "read" multiple packets */
        long overall = 0;
        long offset = 0;
        long trans, left;
        long len;

        if (tx != null)
            len = tx.data.length;
        else
            len = rx.data.length;

        while (len > 0) {
            TransferChunk* chunk;
            /* We have exhausted our data */
            if (replay_chunk >= recording.length)
                return -Posix.ENOENT;

            chunk = &recording[replay_chunk];
            left = -1;
            trans = len;

            if (chunk.tx != null) {
                if (tx == null)
                    return -Posix.ENOMSG;

                if (trans > chunk.tx.length - replay_byte)
                    trans = chunk.tx.length - replay_byte;
                left = chunk.tx.length - replay_byte - trans;

                if (Posix.memcmp(&tx.data[offset], &chunk.tx[replay_byte], trans) != 0)
                    return -Posix.ENOMSG;

                /* We are happy. */
            }

            if (chunk.rx != null) {
                if (rx == null)
                    return -Posix.ENOMSG;

                if (trans > chunk.rx.length - replay_byte)
                    trans = chunk.rx.length - replay_byte;
                left = chunk.rx.length - replay_byte - trans;

                Posix.memcpy(&rx.data[offset], &chunk.rx[replay_byte], trans);
                /* We are happy. */
            }
            assert(left >= 0);

            if (left == 0) {
                replay_chunk += 1;
                replay_byte = 0;
            } else {
                replay_byte += trans;
            }

            offset += trans;
            len -= trans;
            overall += trans;
        }

        return overall;
    }
}

internal class IoctlSpiRecorder : IoctlSpiBase {

    bool cs_is_high;
    Posix.FILE log;

    public IoctlSpiRecorder(MainContext? ctx, string device, string file)
    {
        base (ctx);

        // Open SPI log file
        cs_is_high = false;
        log = Posix.FILE.open(file, "w");

        log.printf("@DEV %s (SPI)\n", device);
    }

    public override bool handle_ioctl(IoctlClient client) {
        IoctlData? data = null;
        ulong request = client.request;
        ulong size = (request >> Ioctl._IOC_SIZESHIFT) & ((1 << Ioctl._IOC_SIZEBITS) - 1);

        try {
            data = client.arg.resolve(0, size, true, true);
        } catch (IOError e) {
            warning("Error resolving IOCtl data: %s", e.message);
            return false;
        }

        if ((request & (~ (((1 << Ioctl._IOC_SIZEBITS) - 1) << Ioctl._IOC_SIZESHIFT))) == Ioctl.SPI_IOC_MESSAGE(0)) {
            int res, my_errno;
            assert(size % sizeof(Ioctl.spi_ioc_transfer) == 0);
            ulong count = size / sizeof(Ioctl.spi_ioc_transfer);

            try {
                res = client.execute(out my_errno);
            } catch (GLib.IOError e) {
                return false;
            };
            if (res < 0) {
                client.complete(-1, my_errno);
                return true;
            }

            client.complete(res, 0);

            iter_ioctl_vector(count, data, true);

            return true;
        } else {
            /* Unhandled currently:
             * Ioctl.SPI_IOC_RD_MODE
             * Ioctl.SPI_IOC_WR_MODE
             * Ioctl.SPI_IOC_RD_MODE32
             * Ioctl.SPI_IOC_WR_MODE32
             * Ioctl.SPI_IOC_RD_LSB_FIRST
             * Ioctl.SPI_IOC_WR_LSB_FIRST
             * Ioctl.SPI_IOC_RD_BITS_PER_WORD
             * Ioctl.SPI_IOC_WR_BITS_PER_WORD
             * Ioctl.SPI_IOC_RD_MAX_SPEED_HZ
             * Ioctl.SPI_IOC_WR_MAX_SPEED_HZ
             */

            client.complete(-1, Posix.ENOTTY);
            return true;
        }
    }

    public override bool handle_read(IoctlClient client) {
        int res, my_errno;

        try {
            res = client.execute(out my_errno);
        } catch (GLib.IOError e) {
            return false;
        };
        if (res < 0) {
            client.complete(-1, my_errno);
            return true;
        }
        /* Read memory is not resolved yet. */
        try {
            client.arg.reload(false);
        } catch (GLib.IOError e) {
            return false;
        };
        client.complete(res, 0);

        handle_read_write(null, client.arg, false);

        return true;
    }

    public override bool handle_write(IoctlClient client) {
        int res, my_errno;

        try {
            res = client.execute(out my_errno);
        } catch (GLib.IOError e) {
            return false;
        };
        if (res < 0) {
            client.complete(-1, my_errno);
            return true;
        }
        client.complete(res, 0);

        handle_read_write(client.arg, null, false);

        return true;
    }

    void write_hex(uint8[] buf) {
        foreach (uint8 c in buf) {
            log.printf("%02x", c);
        }
    }

    internal override long handle_read_write(IoctlData? tx, IoctlData? rx, bool keep_cs_high) {
        bool indent = false;

        if (cs_is_high)
            log.printf("C");
        else
            log.printf("T");

        if (tx != null) {
            log.printf("W ");
            write_hex(tx.data);
            log.printf("\n");

            indent = true;
        }
        if (rx != null) {
            if (indent)
                log.printf(" ");
            log.printf("R ");
            write_hex(rx.data);
            log.printf("\n");
        }

        cs_is_high = keep_cs_high;

        return 0;
    }
}

}
