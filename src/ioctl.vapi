[CCode (cprefix = "", lower_case_cprefix = "")]
namespace Ioctl {
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_CLAIMINTERFACE;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_GETDRIVER;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_CONNECTINFO;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_SUBMITURB;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_REAPURB;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_REAPURBNDELAY;
    [CCode (cheader_filename = "sys/ioctl.h")]
    public const int USBDEVFS_DISCARDURB;
    [CCode (cheader_filename = "sys/ioctl.h")]
    public const int USBDEVFS_GET_CAPABILITIES;
    [CCode (cheader_filename = "sys/ioctl.h")]
    public const int USBDEVFS_RELEASEINTERFACE;
    [CCode (cheader_filename = "sys/ioctl.h")]
    public const int USBDEVFS_CLEAR_HALT;
    [CCode (cheader_filename = "sys/ioctl.h")]
    public const int USBDEVFS_RESET;
    [CCode (cheader_filename = "sys/ioctl.h")]
    public const int USBDEVFS_RESETEP;
    [CCode (cheader_filename = "sys/ioctl.h")]
    public const int TIOCSBRK;

    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_CAP_ZERO_PACKET;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_CAP_BULK_CONTINUATION;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_CAP_NO_PACKET_SIZE_LIM;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_CAP_BULK_SCATTER_GATHER;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_CAP_REAP_AFTER_DISCONNECT;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_CAP_MMAP;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_CAP_DROP_PRIVILEGES;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_CAP_CONNINFO_EX;
    [CCode (cheader_filename = "linux/usbdevice_fs.h")]
    public const int USBDEVFS_CAP_SUSPEND;

    [CCode (cheader_filename = "asm-generic/ioctl.h")]
    public const int _IOC_SIZEBITS;
    [CCode (cheader_filename = "asm-generic/ioctl.h")]
    public const int _IOC_SIZESHIFT;
    [CCode (cheader_filename = "asm-generic/ioctl.h")]
    public const int _IOC_TYPEBITS;
    [CCode (cheader_filename = "asm-generic/ioctl.h")]
    public const int _IOC_TYPESHIFT;

    [CCode (cname="struct usbdevfs_connectinfo", cheader_filename = "linux/usbdevice_fs.h")]
    public struct usbdevfs_connectinfo {
	public uint devnum;
	public uint slow;
    }

    [CCode (cname="struct usbdevfs_urb", cheader_filename = "linux/usbdevice_fs.h",
            destroy_function="")]
    public struct usbdevfs_urb {
	uint8 type;
	uint8 endpoint;
	int status;
	uint flags;
        [CCode (array_length=false)]
	uint8[] buffer;
	int buffer_length;
	int actual_length;
	int start_frame;
	int number_of_packets;
	int error_count;
	uint signr;
	void *usercontext;
    }
}

