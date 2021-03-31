
namespace UMockdevUtils {

// Recursively remove a directory and all its contents.
public void
remove_dir (string path, bool remove_toplevel=true)
{
    if (FileUtils.test(path, FileTest.IS_DIR) && !FileUtils.test(path, FileTest.IS_SYMLINK)) {
        Dir d;
        try {
            d = Dir.open(path, 0);
        } catch (FileError e) {
            warning("cannot open: %s: %s", path, e.message);
            return;
        }

        string name;
        while ((name = d.read_name()) != null)
            remove_dir (Path.build_filename(path, name), true);
    }

    if (remove_toplevel)
        if (FileUtils.remove(path) < 0)
            warning("cannot remove %s: %s", path, strerror(errno));
}

}
