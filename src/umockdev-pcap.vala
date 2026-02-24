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
using pcap;

const int URB_TRANSFER_IN = 0x80;
const int URB_ISOCHRONOUS = 0x0;
const int URB_INTERRUPT = 0x1;
const int URB_CONTROL = 0x2;
const int URB_BULK = 0x3;

private string urb_type_to_string(int type)
{
    switch (type) {
        case URB_INTERRUPT:
            return "INTERRUPT";
        case URB_CONTROL:
            return "CONTROL";
        case URB_BULK:
            return "BULK";
        default:
            return "UNKNOWN (%d)".printf(type);
    }
}

private struct UrbInfo {
    IoctlData urb_data;
    IoctlData buffer_data;
    uint64 pcap_id;
}


internal class IoctlUsbPcapHandler : IoctlBase {

    /* Make up some capabilities (that have useful properties) */
    const uint32 capabilities = USBDEVFS_CAP_BULK_SCATTER_GATHER |
                                USBDEVFS_CAP_BULK_CONTINUATION |
                                USBDEVFS_CAP_NO_PACKET_SIZE_LIM |
                                USBDEVFS_CAP_REAP_AFTER_DISCONNECT |
                                USBDEVFS_CAP_ZERO_PACKET;
    private pcap.pcap rec;
    private Array<UrbInfo?> urbs;
    private Array<UrbInfo?> discarded;
    private int bus;
    private int device;

    public IoctlUsbPcapHandler(string file, int bus, int device)
    {
        char errbuf[pcap.ERRBUF_SIZE];
        base ();

        this.bus = bus;
        this.device = device;

        rec = new pcap.pcap.open_offline(file, errbuf);

        if (rec.datalink() != dlt.USB_LINUX_MMAPPED)
            error("Only DLT_USB_LINUX_MMAPPED recordings are supported!");

        urbs = new Array<UrbInfo?>();
        discarded = new Array<UrbInfo?>();
    }

    public override bool handle_ioctl(IoctlClient client) {
        IoctlData? data = null;
        ulong request = client.request;
        ulong size = (request >> Ioctl._IOC_SIZESHIFT) & ((1 << Ioctl._IOC_SIZEBITS) - 1);

        try {
            data = client.arg.resolve(0, size);
        } catch (IOError e) {
            warning("Error resolving IOCtl data: %s", e.message);
            return false;
        }

        switch (request) {
            case USBDEVFS_GET_CAPABILITIES:
                *(uint32*) data.data = capabilities;

                client.complete(0, 0);
                return true;

            case USBDEVFS_CLAIMINTERFACE:
            case USBDEVFS_RELEASEINTERFACE:
            case USBDEVFS_CLEAR_HALT:
            case USBDEVFS_RESET:
            case USBDEVFS_RESETEP:
                client.complete(0, 0);
                return true;

            case USBDEVFS_DISCARDURB:
                for (int i = 0; i < urbs.length; i++) {
                    if (urbs.index(i).urb_data.client_addr == *((ulong*)client.arg.data)) {
                        /* Found the urb, add to discard array, remove it and return success */
                        discarded.prepend_val(urbs.index(i));
                        urbs.remove_index(i);
                        client.complete(0, 0);
                        return true;
                    }
                }

                client.complete(-1, Posix.EINVAL);
                return true;


            case USBDEVFS_SUBMITURB:
                /* Just put the urb information into our queue (but resolve the buffer). */
                Ioctl.usbdevfs_urb *urb = (Ioctl.usbdevfs_urb*) data.data;
                size_t offset = (ulong) &urb.buffer - (ulong) urb;
                UrbInfo info = { };

                info.urb_data = data;
                try {
                    info.buffer_data = data.resolve(offset, urb.buffer_length);
                } catch (IOError e) {
                    warning("Error resolving IOCtl data: %s", e.message);
                    return false;
                }
                info.pcap_id = 0;

                urbs.append_val(info);
                client.complete(0, 0);
                return true;

            case USBDEVFS_REAPURB:
            case USBDEVFS_REAPURBNDELAY:
                UrbInfo? urb_info = null;
                if (discarded.length > 0) {
                    urb_info = discarded.index(0);
                    discarded.remove_index(0);

                    Ioctl.usbdevfs_urb *urb = (Ioctl.usbdevfs_urb*) urb_info.urb_data.data;
                    urb.status = -Posix.ENOENT;

                    /* Warn if we are discarding an urb that had no matching submit
                     * in the recording. The replay may be stuck at this point and
                     * we are timing out on URBs that will not replay.
                     */
                    if (urb_info.pcap_id == 0) {
                        message("Replay may be stuck: Reaping discard URB of type %s, for endpoint 0x%02x with length %d without corresponding submit",
                                urb_type_to_string(urb.type), urb.endpoint, urb.buffer_length);
                    }
                } else {
                    urb_info = next_reapable_urb();
                }

                if (urb_info != null) {
                     data.set_ptr(0, urb_info.urb_data);
                     client.complete(0, 0);
                     return true;
                } else {
                    client.complete(-1, Posix.EAGAIN);
                    return true;
                }

            default:
                client.complete(-1, Posix.ENOTTY);
                return true;
        }
    }

    /* If we are stuck, we need to be able to look at the already fetched
     * packet. As such, keep it in a global state.
     */
    private pcap.pkthdr cur_hdr;
    private uint64 start_time_ms;
    private uint64 last_pkt_time_ms;
    private uint64 cur_waiting_since;
    private unowned uint8[]? cur_buf = null;

    private UrbInfo? next_reapable_urb() {
        bool debug = false;
        uint64 now = GLib.get_monotonic_time();

        /* Immediately exit if we have no urbs that could be reaped.
         * This is important, as we might incorrectly skip control transfers
         * otherwise.
         */
        if (urbs.length == 0)
            return null;

        /* Fetch the first packet if we do not have one. */
        if (cur_buf == null) {
            cur_buf = rec.next(ref cur_hdr);

            if (cur_buf == null)
                return null;

            usb_header_mmapped *urb_hdr = (void*) cur_buf;

            cur_waiting_since = now;
            last_pkt_time_ms = uint64.from_little_endian(urb_hdr.ts_sec) * 1000 + uint32.from_little_endian(urb_hdr.ts_usec) / 1000;
            start_time_ms = last_pkt_time_ms;
        }

        for (; cur_buf != null; cur_buf = rec.next(ref cur_hdr), cur_waiting_since = now) {
            assert(cur_hdr.caplen >= 64);

            usb_header_mmapped *urb_hdr = (void*) cur_buf;

            uint64 cur_pkt_time_ms = uint64.from_little_endian(urb_hdr.ts_sec) * 1000 + uint32.from_little_endian(urb_hdr.ts_usec) / 1000;

            /* Discard anything from a different bus/device */
            if (uint16.from_little_endian(urb_hdr.bus_id) != bus || urb_hdr.device_address != device)
                continue;

            /* Print out debug info, if we need 5s longer than the recording
             * (to aovid printing debug info if we are replaying a timeout)
             */
            if ((now - cur_waiting_since) / 1000 > 2000 + (cur_pkt_time_ms - last_pkt_time_ms)) {
                message("Stuck for %lu ms, recording needed %lu ms",
                        (ulong) (now - cur_waiting_since) / 1000,
                        (ulong) (cur_pkt_time_ms - last_pkt_time_ms));
                message("Trying to reap at recording position %c %s packet, for endpoint 0x%02x with length %u, replay may be stuck (time: %.3f)",
                        urb_hdr.event_type, urb_type_to_string(urb_hdr.transfer_type), urb_hdr.endpoint_number, uint32.from_little_endian(urb_hdr.urb_len), (cur_pkt_time_ms - start_time_ms) / 1000.0);
                message("The device has currently %u in-flight URBs:", urbs.length);

                for (var i = 0; i < urbs.length; i++) {
                    unowned UrbInfo? urb_data = urbs.index(i);
                    Ioctl.usbdevfs_urb *urb = (Ioctl.usbdevfs_urb*) urb_data.urb_data.data;

                    message("   %s URB, for endpoint 0x%02x with length %d; %ssubmitted",
                            urb_type_to_string(urb.type), urb.endpoint, urb.buffer_length,
                            urb_data.pcap_id == 0 ? "NOT " : "");
                }
                cur_waiting_since = now;
                debug = true;
            }

            /* Submit */
            if (urb_hdr.event_type == 'S') {
                /* Check each pending URB (in oldest to newest order) and see
                 * if the information matches, and if yes, we mark the urb as
                 * submitted (and therefore reapable).
                 */
                int i;
                for (i = 0; i < urbs.length; i++) {
                    unowned UrbInfo? urb_data = urbs.index(i);
                    Ioctl.usbdevfs_urb *urb = (Ioctl.usbdevfs_urb*) urb_data.urb_data.data;

                    /* Urb already submitted. */
                    if (urb_data.pcap_id != 0)
                        continue;

                    uint8* urb_buffer = urb.buffer;
                    int urb_buffer_length = urb.buffer_length;

                    /* For control transfers, the start of the buffer is the
                     * setup data, which is stored separately in the capture.
                     */
                    if (urb.type == URB_CONTROL) {
                        assert(urb_buffer_length >= 8);
                        urb_buffer = &urb.buffer[8];
                        urb_buffer_length -= 8;
                    }

                    /* libusb always sets URB_CONTROL endpoint to 0x00, but the
                     * kernel exposes it as 0x80/0x00 depending on the direction
                     */
                    if ((urb.type != urb_hdr.transfer_type) ||
                        ((urb.type != URB_CONTROL) && (urb.endpoint != urb_hdr.endpoint_number)) ||
                        (urb_buffer_length != uint32.from_little_endian(urb_hdr.urb_len))) {

                        if (debug)
                            stderr.printf("UMockdev: Queued URB %d has a metadata mismatch!\n", i);
                        continue;
                    }

                    /* Compare the setup data (first 8 bytes of buffer) */
                    if (urb.type == URB_CONTROL && urb_hdr.setup_flag == 0) {
                        if (Posix.memcmp(urb.buffer, &urb_hdr.s, 8) != 0)
                            continue;
                    }

                    if (uint32.from_little_endian(urb_hdr.data_len) > 0) {
                        /* Data must have been captured. */
                        assert(urb_hdr.data_flag == 0);
                        assert(uint32.from_little_endian(urb_hdr.data_len) == urb_buffer_length);

                        /* Compare the full buffer (as we are outgoing) */
                        if (Posix.memcmp(urb_buffer, &cur_buf[sizeof(usb_header_mmapped)], urb_buffer_length) != 0) {
                            if (debug) {
                                stderr.printf("UMockdev: Queued URB %d has a buffer mismatch! Recording:", i);
                                for (int j = 0; j < urb_buffer_length; j++) {
                                    if (j > 0 && j % 8 == 0)
                                        stderr.printf("\n");
                                    stderr.printf(" %02x", cur_buf[sizeof(usb_header_mmapped) + j]);
                                }
                                stderr.printf("\nUMockdev: Submitted:");
                                for (int j = 0; j < urb_buffer_length; j++) {
                                    if (j > 0 && j % 8 == 0)
                                        stderr.printf("\n");
                                    stderr.printf(" %02x", urb_buffer[j]);
                                }
                                stderr.printf("\n");
                            }
                            continue;
                        }
                    }

                    /* Everything matches, mark as submitted */
                    urb_data.pcap_id = uint64.from_little_endian(urb_hdr.id);

                    /* Packet was handled. */
                    last_pkt_time_ms = uint64.from_little_endian(urb_hdr.ts_sec) * 1000 + uint32.from_little_endian(urb_hdr.ts_usec) / 1000;
                    break;
                }

                /* Found a packet, continue! */
                if (i != urbs.length)
                    continue;
            } else {
                UrbInfo? urb_info = null;
                Ioctl.usbdevfs_urb *urb = null;
                /* 'C' or 'E'; we don't implement errors yet */

                assert(urb_hdr.event_type == 'C');

                for (int i = 0; i < urbs.length; i++) {
                    urb_info = urbs.index(i);

                    if (urb_info.pcap_id == uint64.from_little_endian(urb_hdr.id)) {
                        urb = (Ioctl.usbdevfs_urb*) urb_info.urb_data.data;
                        urbs.remove_index(i);
                        break;
                    }

                    urb_info = null;
                }

                /* We don't have a submit node for this urb.
                 * Just ignore it as it is probably a control transfer that was
                 * initiated by the kernel. */
                if (urb == null)
                    continue;

                /* We can reap this urb!
                 * Copy any data back if present.
                 */
                if (uint32.from_little_endian(urb_hdr.data_len) > 0) {
                    assert(urb_hdr.data_flag == 0);

                    uint8* urb_buffer = urb.buffer;

                    /* For control transfers, the start of the buffer is the
                     * setup data, which is stored separately in the capture.
                     */
                    if (urb.type == URB_CONTROL)
                        urb_buffer = &urb.buffer[8];

                    Posix.memcpy(urb_buffer, &cur_buf[sizeof(usb_header_mmapped)], uint32.from_little_endian(urb_hdr.data_len));
                }
                urb.status = (int) int32.from_little_endian(urb_hdr.status);
                urb.actual_length = (int) uint32.from_little_endian(urb_hdr.urb_len);

                /* Does this need further handling? */
                assert(uint32.from_little_endian(urb_hdr.start_frame) == 0);
                urb.start_frame = (int) uint32.from_little_endian(urb_hdr.start_frame);

                last_pkt_time_ms = uint64.from_little_endian(urb_hdr.ts_sec) * 1000 + uint32.from_little_endian(urb_hdr.ts_usec) / 1000;

                return urb_info;
            }

            /* Packet not handled.
             * Now, if it was a control transfer of "standard" type, then we
             * should just ignore it (and ignore the subsequent response).
             *
             * Note that we use a different mechanism to ignore the replies.
             */
            if (urb_hdr.transfer_type == URB_CONTROL) {
                /* 0x60 -> any direction, standard transfer, any recepient */
                if (urb_hdr.event_type == 'S' && urb_hdr.setup_flag == 0 && ((*(uint8*)&urb_hdr.s) & 0x60) == 0x00)
                    continue;
            }

            /* The current packet cannot be reaped at this point, give up. */
            return null;
        }

        return null;
    }
}

}
