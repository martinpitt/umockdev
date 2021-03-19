[CCode (lower_case_cprefix = "pcap_", cheader_filename = "pcap/pcap.h")]
namespace pcap {

const int ERRBUF_SIZE;

[Compact]
[CCode (cname="pcap_t", lower_case_cprefix = "pcap_", free_function="pcap_close")]
public class pcap {
    [CCode (cname="pcap_open_offline")]
    public pcap.open_offline(string filename, [CCode (array_length = false)] char[]? errbuf = null);
    [CCode (cname="pcap_fopen_offline")]
    public pcap.fopen_offline(Posix.FILE f, [CCode (array_length = false)] char[]? errbuf = null);

    public int datalink();

    [CCode (array_length = false)]
    public unowned uint8[]? next(ref pkthdr h);
}

[Compact]
[CCode (cname="struct pcap_pkthdr")]
public struct pkthdr {
    Posix.timeval ts;
    uint32 caplen;
    uint32 len;
}

/* Representing the of ; not a direct copy from the usb.h header */
[CCode (cname="pcap_usb_header_mmapped", cheader_filename = "pcap/usb.h")]
public struct usb_header_mmapped {
    uint64 id;

    uint8 event_type;
    uint8 transfer_type;
    uint8 endpoint_number;
    uint8 device_address;
    uint16 bus_id;
    uint8 setup_flag;
    uint8 data_flag;

    uint64 ts_sec;

    uint32 ts_usec;
    int32 status;

    uint32 urb_len;
    uint32 data_len;

    uint64 s; /* Really a union of setup/iso information */

    uint32 interval;
    uint32 start_frame;
    uint32 transfer_flags;
    uint32 iso_numdesc;
}

}

/* This is a separate namespace because cprefix= does not seem to work */
[CCode (cheader_filename = "pcap/dlt.h")]
namespace dlt {
/* data link classes */
const int NULL;
const int EN10MB;
const int EN3MB;
const int AX25;
const int PRONET;
const int CHAOS;
const int IEEE802;
const int ARCNET;
const int SLIP;
const int PPP;
const int FDDI;
const int ATM_RFC1483;
const int RAW;
const int SLIP_BSDOS;
const int PPP_BSDOS;
const int PFSYNC;
const int ATM_CLIP;
const int REDBACK_SMARTEDGE;
const int PPP_SERIAL;
const int PPP_ETHER;
const int SYMANTEC_FIREWALL;
const int MATCHING_MIN;
const int C_HDLC;
const int CHDLC;
const int IEEE802_11;
const int FRELAY;
const int LOOP;
const int ENC;
const int LINUX_SLL;
const int LTALK;
const int ECONET;
const int IPFILTER;
const int PFLOG;
const int CISCO_IOS;
const int PRISM_HEADER;
const int AIRONET_HEADER;
const int HHDLC;
const int IP_OVER_FC;
const int SUNATM;
const int RIO;
const int PCI_EXP;
const int AURORA;
const int IEEE802_11_RADIO;
const int TZSP;
const int ARCNET_LINUX;
const int JUNIPER_MLPPP;
const int JUNIPER_MLFR;
const int JUNIPER_ES;
const int JUNIPER_GGSN;
const int JUNIPER_MFR;
const int JUNIPER_ATM2;
const int JUNIPER_SERVICES;
const int JUNIPER_ATM1;
const int APPLE_IP_OVER_IEEE1394;
const int MTP2_WITH_PHDR;
const int MTP2;
const int MTP3;
const int SCCP;
const int DOCSIS;
const int LINUX_IRDA;
const int IBM_SP;
const int IBM_SN;
const int USER0;
const int USER1;
const int USER2;
const int USER3;
const int USER4;
const int USER5;
const int USER6;
const int USER7;
const int USER8;
const int USER9;
const int USER10;
const int USER11;
const int USER12;
const int USER13;
const int USER14;
const int USER15;
const int IEEE802_11_RADIO_AVS;
const int JUNIPER_MONITOR;
const int BACNET_MS_TP;
const int PPP_PPPD;
const int PPP_WITH_DIRECTION;
const int LINUX_PPP_WITHDIRECTION;
const int JUNIPER_PPPOE;
const int JUNIPER_PPPOE_ATM;
const int GPRS_LLC;
const int GPF_T;
const int GPF_F;
const int GCOM_T1E1;
const int GCOM_SERIAL;
const int JUNIPER_PIC_PEER;
const int ERF_ETH;
const int ERF_POS;
const int LINUX_LAPD;
const int JUNIPER_ETHER;
const int JUNIPER_PPP;
const int JUNIPER_FRELAY;
const int JUNIPER_CHDLC;
const int MFR;
const int JUNIPER_VP;
const int A429;
const int A653_ICM;
const int USB_FREEBSD;
const int USB;
const int BLUETOOTH_HCI_H4;
const int IEEE802_16_MAC_CPS;
const int USB_LINUX;
const int CAN20B;
const int IEEE802_15_4_LINUX;
const int PPI;
const int IEEE802_16_MAC_CPS_RADIO;
const int JUNIPER_ISM;
const int IEEE802_15_4_WITHFCS;
const int IEEE802_15_4;
const int SITA;
const int ERF;
const int RAIF1;
const int IPMB_KONTRON;
const int JUNIPER_ST;
const int BLUETOOTH_HCI_H4_WITH_PHDR;
const int AX25_KISS;
const int LAPD;
const int PPP_WITH_DIR;
const int C_HDLC_WITH_DIR;
const int FRELAY_WITH_DIR;
const int LAPB_WITH_DIR;
const int IPMB_LINUX;
const int FLEXRAY;
const int MOST;
const int LIN;
const int X2E_SERIAL;
const int X2E_XORAYA;
const int IEEE802_15_4_NONASK_PHY;
const int LINUX_EVDEV;
const int GSMTAP_UM;
const int GSMTAP_ABIS;
const int MPLS;
const int USB_LINUX_MMAPPED;
const int DECT;
const int AOS;
const int WIHART;
const int FC_2;
const int FC_2_WITH_FRAME_DELIMS;
const int IPNET;
const int CAN_SOCKETCAN;
const int IPV4;
const int IPV6;
const int IEEE802_15_4_NOFCS;
const int DBUS;
const int JUNIPER_VS;
const int JUNIPER_SRX_E2E;
const int JUNIPER_FIBRECHANNEL;
const int DVB_CI;
const int MUX27010;
const int STANAG_5066_D_PDU;
const int JUNIPER_ATM_CEMIC;
const int NFLOG;
const int NETANALYZER;
const int NETANALYZER_TRANSPARENT;
const int IPOIB;
const int MPEG_2_TS;
const int NG40;
const int NFC_LLCP;
const int INFINIBAND;
const int SCTP;
const int USBPCAP;
const int RTAC_SERIAL;
const int BLUETOOTH_LE_LL;
const int WIRESHARK_UPPER_PDU;
const int NETLINK;
const int BLUETOOTH_LINUX_MONITOR;
const int BLUETOOTH_BREDR_BB;
const int BLUETOOTH_LE_LL_WITH_PHDR;
const int PROFIBUS_DL;
const int PKTAP;
const int EPON;
const int IPMI_HPM_2;
const int ZWAVE_R1_R2;
const int ZWAVE_R3;
const int WATTSTOPPER_DLM;
const int ISO_14443;
const int RDS;
const int USB_DARWIN;
const int OPENFLOW;
const int SDLC;
const int TI_LLN_SNIFFER;
const int LORATAP;
const int VSOCK;
const int NORDIC_BLE;
const int DOCSIS31_XRA31;
const int ETHERNET_MPACKET;
const int DISPLAYPORT_AUX;
const int LINUX_SLL2;
const int SERCOS_MONITOR;
const int OPENVIZSLA;
const int EBHSCR;
const int VPP_DISPATCH;
const int DSA_TAG_BRCM;
const int DSA_TAG_BRCM_PREPEND;
const int IEEE802_15_4_TAP;
const int DSA_TAG_DSA;
const int DSA_TAG_EDSA;
const int ELEE;
const int Z_WAVE_SERIAL;
const int USB_2_0;
const int ATSC_ALP;
const int MATCHING_MAX;

}
