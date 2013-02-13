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

