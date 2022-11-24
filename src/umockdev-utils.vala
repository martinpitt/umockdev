
namespace UMockdevUtils {

public void
exit_error(string message, ...)
{
    stderr.vprintf(message, va_list());
    stderr.puts("\n");
    Process.exit(1);
}

public void
checked_setenv(string variable, string value)
{
    if (!Environment.set_variable(variable, value, true))
        exit_error("Failed to set env variable %s", variable);
}

// only use this in tests, not in runtime code!
public void
checked_remove(string path)
{
    if (FileUtils.remove(path) < 0)
        error("cannot remove %s: %m", path);
}

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
        checked_remove(path);
}

private static Pid process_under_test;
private static ChildWatchFunc process_under_test_watch_cb;

static void
pud_sig_handler (int sig)
{
    if (process_under_test == 0)
        return;

    debug ("umockdev: caught signal %i, propagating to child\n", sig);
    if (Posix.kill (process_under_test, sig) != 0)
        stderr.printf ("umockdev: unable to propagate signal %i to child %i: %s\n",
                       sig, process_under_test, strerror (errno));
}

static void
pud_watch_cb (Pid pid, int status)
{
    /* child is dead, reset state and report */
    if (process_under_test_watch_cb != null) {
        process_under_test_watch_cb (pid, status);
    }

    Process.close_pid (process_under_test);

    process_under_test = 0;
    process_under_test_watch_cb = null;
}

public Pid
spawn_process_under_test(string[] argv, owned ChildWatchFunc watch_cb) throws SpawnError
{
    assert(process_under_test == 0);

    // we want to run opt_program as a subprocess instead of execve()ing, so
    // that we can run device script threads in the background
    Process.spawn_async (null, argv, null,
                         SpawnFlags.SEARCH_PATH | SpawnFlags.CHILD_INHERITS_STDIN | SpawnFlags.DO_NOT_REAP_CHILD ,
                         null, out process_under_test);

    // propagate signals to the child
    var act = Posix.sigaction_t() { sa_handler = pud_sig_handler, sa_flags = Posix.SA_RESETHAND };
#if VALA_0_40
    Posix.sigemptyset (out act.sa_mask);
    assert (Posix.sigaction (Posix.Signal.TERM, act, null) == 0);
    assert (Posix.sigaction (Posix.Signal.HUP, act, null) == 0);
    assert (Posix.sigaction (Posix.Signal.INT, act, null) == 0);
    assert (Posix.sigaction (Posix.Signal.QUIT, act, null) == 0);
    assert (Posix.sigaction (Posix.Signal.ABRT, act, null) == 0);
#else
    Posix.sigemptyset (act.sa_mask);
    assert (Posix.sigaction (Posix.SIGTERM, act, null) == 0);
    assert (Posix.sigaction (Posix.SIGHUP, act, null) == 0);
    assert (Posix.sigaction (Posix.SIGINT, act, null) == 0);
    assert (Posix.sigaction (Posix.SIGQUIT, act, null) == 0);
    assert (Posix.sigaction (Posix.SIGABRT, act, null) == 0);
#endif

    process_under_test_watch_cb = (owned) watch_cb;
    ChildWatch.add(process_under_test, pud_watch_cb);

    return process_under_test;
}

}
