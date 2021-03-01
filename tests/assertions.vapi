[CCode (cprefix = "G", lower_case_cprefix = "g_", cheader_filename = "glib.h")]
namespace Assertions {
#if !VALA_0_52
  public enum CompareOperator {
    [CCode (cname = "==")]
    EQ,
    [CCode (cname = "!=")]
    NE,
    [CCode (cname = "<")]
    LT,
    [CCode (cname = ">")]
    GT,
    [CCode (cname = "<=")]
    LE,
    [CCode (cname = ">=")]
    GE
  }

  public void assert_cmpstr (string? s1, CompareOperator op, string? s2);
  public void assert_cmpint (int n1, CompareOperator op, int n2);
  public void assert_cmpuint (uint n1, CompareOperator op, uint n2);
  public void assert_cmphex (uint n1, CompareOperator op, uint n2);
  public void assert_cmpfloat (float n1, CompareOperator op, float n2);
#endif
}
