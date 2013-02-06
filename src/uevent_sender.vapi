[CCode (lower_case_cprefix = "uevent_", cheader_filename = "uevent_sender.h")]
namespace UeventSender {

  [Compact]
  [CCode (cname="uevent_sender", free_function="uevent_sender_close")]
  public class sender {
      [CCode (cname="uevent_sender_open")]
      public sender (string rootpath);
      public void send (string devpath, string action);
  }
}
