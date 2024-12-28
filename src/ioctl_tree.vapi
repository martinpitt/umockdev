[CCode (lower_case_cprefix = "ioctl_", cheader_filename = "ioctl_tree.h")]
namespace IoctlTree {

  [Compact]
  [CCode (cname="ioctl_tree", free_function="ioctl_tree_free")]
  public class Tree {
      [CCode (cname="ioctl_tree_read")]
      public Tree(Posix.FILE f);
      public Tree.from_bin(ulong id, void *addr, int ret);

      [ReturnsModifiedPointer]
      public void insert(owned Tree node);
      public void* execute(void* last, ulong id, void* addr, out int ret);
      [CCode (instance_pos = -1)]
      public void write(Posix.FILE f);

      public int next_ret(void* last);
  }

  public int data_size_by_id(ulong id);
}
