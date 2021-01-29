[CCode (lower_case_cprefix = "ioctl_", cheader_filename = "ioctl_tree.h")]
namespace IoctlTree {

  [Compact]
  [CCode (cname="ioctl_tree", free_function="ioctl_tree_free")]
  public class Tree {
      [CCode (cname="ioctl_tree_read")]
      public Tree(Posix.FILE f);

      public void* execute(void* last, ulong id, void* addr, ref int ret);
  }

  public int data_size_by_id(ulong id);
}
