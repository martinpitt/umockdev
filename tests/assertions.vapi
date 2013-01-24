[CCode (cprefix = "G", lower_case_cprefix = "g_", cheader_filename = "glib.h")]
namespace Assertions {
  public enum OperatorType {
    [CCode (cname = "==")]
    EQUAL,
    [CCode (cname = "!=")]
    NOT_EQUAL,
    [CCode (cname = "<")]
    LESS_THAN,
    [CCode (cname = ">")]
    GREATER_THAN,
    [CCode (cname = "<=")]
    LESS_OR_EQUAL,
    [CCode (cname = ">=")]
    GREATER_OR_EQUAL
  }

  public void assert_cmpstr (string? s1, OperatorType op, string? s2);
  public void assert_cmpint (int n1, OperatorType op, int n2);
  public void assert_cmpuint (uint n1, OperatorType op, uint n2);
  public void assert_cmphex (uint n1, OperatorType op, uint n2);
  public void assert_cmpfloat (float n1, OperatorType op, float n2);
}
