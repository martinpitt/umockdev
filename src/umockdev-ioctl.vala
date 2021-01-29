

namespace UMockdev {

/* The bindings are lacking g_signal_accumulator_true_handled, but easy enough
 * to implement here.
 */
internal bool signal_accumulator_true_handled(GLib.SignalInvocationHint ihint,
                                              GLib.Value return_accu,
                                              GLib.Value handler_return,
                                              void* data)
{
    bool handled;
    bool continue_emission;

    handled = handler_return.get_boolean();
    return_accu.set_boolean(handled);
    continue_emission = !handled;

    return continue_emission;
}


/**
 * UMockdevIoctlData:
 *
 * The #UMockdevIoctlData struct is a container designed to resolve the ioctl
 * data parameter. You will be passed an #UMockdevIoctlData container with
 * a single pointer initially, which you must immediately resolve using
 * umockdev_ioctl_data_resolve() passing 0 as the offset and the expected size
 * of the user provided structure.
 */
public class IoctlData {
    [CCode(array_length_cname="data_len")]
    public uint8[] data;

    private bool parent_reloaded;
    private bool need_flush;
    private ulong client_addr;

    private IOStream stream;

    private IoctlData[] children;
    private size_t[] children_offset;

    internal IoctlData(IOStream stream)
    {
        this.stream = stream;
    }

    /**
     * umockdev_ioctl_data_resolve():
     * @self: A #UmockdevIoctlData
     * @offset: Byte offset of pointer inside data
     * @len: Length of the to be resolved data
     * @load: Load memory from client
     * @dirty: Write memory to client when done
     *
     * Resolve and address inside the data. After this operation, the pointer
     * inside data points to a local copy or a zero initilized memory depending
     * on whether @load is set.
     *
     * You may call this multiple times on the same pointer in order to fetch
     * the existing information.
     */
    public IoctlData? resolve(size_t offset, size_t len, bool load, bool dirty) throws IOError {
        IoctlData res;

        for (int i = 0; i < children.length; i++) {
            if (children_offset[i] == offset) {
                if (load && children[i].parent_reloaded)
                    children[i].reload(false);

                if (dirty)
                    children[i].dirty(false);

                return children[i];
            }
        }

        if (offset + sizeof(size_t) > data.length)
            return null;

        res = new IoctlData(stream);
        res.data = new uint8[len];
        res.need_flush = dirty;
        res.client_addr = *((size_t*) &data[offset]);

        children += res;
        children_offset += offset;

        /* Don't try to resolve null pointers. */
        if (res.client_addr == 0 || len == 0)
            return null;

        *((size_t*) &data[offset]) = (size_t) res.data;

        if (load)
            res.load_data();

        return res;
    }

    /**
     * umockdev_ioctl_data_set_ptr():
     * @self: A #UmockdevIoctlData
     * @offset: Byte offset of pointer inside data
     * @child: Memory block that the pointer should point to
     *
     * Basically the reverse operation of umockdev_ioctl_data_resolve(). It sets
     * the pointer at @offset to point to the data from @child in a way that
     * can be synchronised back to the client.
     *
     * Use of this is rare, but e.g. required to reap USB URBs.
     *
     * The function will only work correctly for pointer elements that have not
     * been resolved before.
     */
    public void set_ptr(size_t offset, IoctlData child) throws IOError {

        foreach (size_t o in children_offset) {
            assert(o != offset);
        }

        assert(offset + sizeof(size_t) <= data.length);

        children += child;
        children_offset += offset;

        *((size_t*) &data[offset]) = (size_t) child.data;
        need_flush = true;
    }

    /**
     * umockdev_ioctl_data_reload():
     * @self: A #UmockdevIoctlData
     * @recurse: Whether to recursively reload children
     *
     * Use this function after executing an ioctl from the client using
     * umockdev_ioctl_client_execute() or if you kept data from a previous
     * ioctl to refetch the memory. It will reload the data of this buffer
     * and resolvse known pointers within. The pointed to buffers will also
     * be reloaded if requested.
     *
     * Note pointers modified cannot be reloaded. As such, you may need to
     * resolve the data again and it is usually a good idea to do so.
     */
    public void reload(bool recurse) throws IOError {
        load_data();

        IoctlData[] old_children = children;
        size_t[] old_offsets = children_offset;

        children.resize(0);
        children_offset.resize(0);

        for (int i = 0; i < old_children.length; i++) {
            /* If the pointer has an unexpected value, then just drop the reference. */
            if (*((size_t*) &data[old_offsets[i]]) != old_children[i].client_addr)
                continue;

            /* Reload and set correct pointer. */
            if (recurse)
                old_children[i].reload(recurse);
            else
                old_children[i].parent_reloaded = true;

            *((void**) &data[old_offsets[i]]) = old_children[i].data;

            /* Store in our new children array. */
            children += old_children[i];
            children_offset += old_offsets[i];
        }
    }

    public void dirty(bool recursive) {
        need_flush = true;

        if (!recursive)
            return;

        foreach (unowned IoctlData child in children)
            child.dirty(true);
    }

    private void load_data() throws IOError {
        OutputStream output = stream.get_output_stream();
        InputStream input = stream.get_input_stream();
        ulong args[3];

        args[0] = 5; /* READ_MEM */
        args[1] = client_addr;
        args[2] = data.length;

        output.write_all((uint8[])args, null, null);
        input.read_all(data, null, null);
    }

    /*
     * Recursively flushes out all data elements, transparently ensuring that
     * pointers are submitted in terms of the client.
     */
    internal async void flush() throws GLib.Error {
        uint8[] submit_data = data;

        for (int i = 0; i < children.length; i++) {
            yield children[i].flush();

            *((size_t*) &submit_data[children_offset[i]]) = children[i].client_addr;
        }

        if (need_flush) {
            OutputStream output = stream.get_output_stream();
            ulong args[3];

            args[0] = 6; /* WRITE_MEM */
            args[1] = client_addr;
            args[2] = submit_data.length;

            yield output.write_all_async((uint8[])args, 0, null, null);
            yield output.write_all_async(submit_data, 0, null, null);

            need_flush = false;
        }
    }

    internal void flush_sync() throws IOError {
        uint8[] submit_data = data;

        for (int i = 0; i < children.length; i++) {
            children[i].flush_sync();

            *((ulong*) &submit_data[children_offset[i]]) = children[i].client_addr;
        }

        if (need_flush) {
            OutputStream output = stream.get_output_stream();
            ulong args[3];

            args[0] = 6; /* WRITE_MEM */
            args[1] = client_addr;
            args[2] = submit_data.length;

            output.write_all((uint8[])args, null, null);
            output.write_all(submit_data, null, null);

            need_flush = false;
        }
    }
}

/**
 * UMockdevIoctlClient:
 *
 * The #UMockdevIoctlClient struct represents an opened client side FD in order
 * to emulate ioctl calls on this device.
 */
public class IoctlClient : GLib.Object {
    private IoctlBase handler;
    private IOStream stream;
    public string devnode { get; }

    public ulong request { get; }
    public IoctlData arg { get; }
    public bool connected {
      get {
        return !stream.is_closed();
      }
    }

    private bool _abort;
    private long result;
    private int result_errno;

    static construct {
        GLib.Signal.@new("handle-ioctl", typeof(IoctlClient), GLib.SignalFlags.RUN_LAST, 0, signal_accumulator_true_handled, null, null, typeof(bool), 0);
    }

    /**
     * umockdev_ioctl_client_execute():
     * @errno_: Return location for errno
     *
     * Execute the ioctl on the client side. Note that this flushes any
     * modifications of the ioctl data. As such, data needs to be re-fetched
     * afterwards.
     *
     * It is only valid to call this while an uncompleted ioctl is being
     * processed.
     *
     * This call is thread-safe.
     *
     * Returns: The client side result of the ioctl call
     */
    public int execute(out int errno_) throws IOError {
        OutputStream output = stream.get_output_stream();
        InputStream input = stream.get_input_stream();
        ulong args[3];

        assert(_request != 0);

        arg.flush_sync();

        args[0] = 4; /* RUN (original ioctl) */
        args[1] = 0; /* unused */
        args[2] = 0; /* unused */

        output.write_all((uint8[])args, null, null);
        input.read_all((uint8[])args, null, null);

        assert(args[0] == 2); /* RES (result) */

        errno_ = (int) args[2];

        return (int) args[1];
    }

    /**
     * umockdev_ioctl_invocation_complete():
     * @self: A #UmockdevIoctlClient
     * @result: Return value of ioctl
     * @errno_: errno of ioctl
     *
     * Asynchronously completes the ioctl invocation of the client. This is
     * equivalent to calling umockdev_ioctl_complete() with the invocation.
     *
     * This call is thread-safe.
     */
    public void complete(long result, int errno_) {
        IdleSource source;

        /* Nullify some of the request information */
        assert(_request != 0);
        _request = 0;

        this.result = result;
        this.result_errno = errno_;

        /* Push us into the correct main context. */
        source = new IdleSource();
        source.set_callback(complete_idle);
        source.attach(handler.ctx);
    }

    /**
     * umockdev_ioctl_invocation_abort():
     * @self: A #UmockdevIoctlClient
     * @result: Return value of ioctl
     * @errno_: errno of ioctl
     *
     * Asynchronously terminates the child by asking it to execute exit(1).
     *
     * This call is thread-safe.
     */
    public void abort() {
        this._abort = true;

        complete(0, 0);
    }

    private async void complete_async() throws GLib.Error {
        OutputStream output = stream.get_output_stream();
        ulong args[3];

        yield _arg.flush();

        if (!_abort) {
            args[0] = 3; /* DONE */
            args[1] = result;
            args[2] = result_errno;
        } else {
            args[0] = 0xff; /* ABORT */
            args[1] = 0;
            args[2] = 0;
        }

        /* Nullify request information */
        _request = 0;
        _arg = null;
        result = 0;
        result_errno = 0;

        yield output.write_all_async((uint8[])args, 0, null, null);

        /* And, finally re-queue ourself for receiving. */
        read_ioctl.begin();
    }

    /* Start listening on the given stream.
     * MUST be called from the correct thread! */
    internal async void read_ioctl()
    {
        InputStream input = stream.get_input_stream();
        size_t bytes;
        ulong args[3];

        try {
            yield input.read_all_async((uint8[])args, 0, null, out bytes);
        } catch (GLib.Error e) {
            /* Probably we lost the client, ignore the error and make sure we are closed.
               Closing needs to finish before the coroutine returns. */
            try {
                yield stream.close_async();
            } catch (IOError e) {};
            return;
        }
        if (input.is_closed() || bytes == 0) {
            /* Closing needs to finish before the coroutine returns. */
            try {
                yield stream.close_async();
            } catch (IOError e) {};
            return;
        }

        assert(args[0] == 1);

        _request = args[1];
        _arg = new IoctlData(stream);
        _arg.data = new uint8[sizeof(ulong)];
        *(ulong*) _arg.data = args[2];

        bool handled = false;

        GLib.Signal.emit_by_name(this, "handle-ioctl", out handled);
        if (!handled)
            handled = handler.handle_ioctl(this);

        if (!handled) {
            IoctlTree.Tree tree = null;
            IoctlData? data = null;
            ulong size = (_request >> Ioctl._IOC_SIZESHIFT) & ((1 << Ioctl._IOC_SIZEBITS) - 1);
            ulong type = (_request >> Ioctl._IOC_TYPESHIFT) & ((1 << Ioctl._IOC_TYPEBITS) - 1);
            int ret = -1;
            int my_errno;

            try {
                data = _arg.resolve(0, size, true, true);
            } catch (IOError e) {
                warning("Error resolving IOCtl data: %s", e.message);

                complete(-100, 0);
                return;
            }

            if ((char) type == 'E') {
                Posix.errno = Posix.ENOENT;
            } else {
                Posix.errno = Posix.ENOTTY;
            }
            if (data != null)
                tree.execute(null, _request, (void*) data.data, ref ret);
            else
                tree.execute(null, _request, null, ref ret);
            my_errno = Posix.errno;
            Posix.errno = 0;

            if (ret != -1) {
                my_errno = 0;
            }

            complete(ret, my_errno);
        }
    }

    private bool complete_idle()
    {
        /* Set completed early, so that an IO error will not trigger the
         * warning from the destructor.
         */
        complete_async.begin();

        return false;
    }

    private bool notify_closed_idle()
    {
        notify_property("connected");
        return false;
    }

    private void notify_closed_cb()
    {
        /* Force into correct thread. */
        handler.ctx.invoke(notify_closed_idle);
    }

    internal IoctlClient(IoctlBase handler, IOStream stream, string devnode)
    {
        this.handler = handler;
        this.stream = stream;
        this._devnode = devnode;

        /* FIXME: There must be a better way to do this in vala? */
        GLib.Signal.connect_object(this.stream, "notify::closed", (GLib.Callback) notify_closed_cb, this, GLib.ConnectFlags.SWAPPED);
    }

    ~IoctlClient()
    {
        if (!stream.is_closed())
            critical("Destroying IoctlClient with open stream!");
    }
}


/**
 * UMockdevIoctlBase:
 *
 * The #UMockdevIoctlBase class is a base class to emulate and record ioctl
 * operations of a client. It can be attached to an emulated device in the
 * testbed and will then be used.
 */

private class StartListenClosure {
    public IoctlBase handler;
    public SocketListener listener;
    public string devnode;

    public StartListenClosure(IoctlBase handler, SocketListener listener, string devnode) {
        this.handler = handler;
        this.listener = listener;
        this.devnode = devnode;
    }

    public bool cb() {
        handler.socket_listen.begin(listener, devnode);
        return false;
    }
}

public class IoctlBase: GLib.Object {
    public MainContext ctx { get; set construct; }
    private HashTable<string,Cancellable> listeners;

    static construct {
        GLib.Signal.@new("handle-ioctl", typeof(IoctlBase), GLib.SignalFlags.RUN_LAST, 0, signal_accumulator_true_handled, null, null, typeof(bool), 1, typeof(IoctlClient));
    }

    construct {
        /* XXX: Should we use the default or thread_default? */
        if (ctx == null)
            ctx = GLib.MainContext.@default();
        listeners = new HashTable<string,Cancellable>(str_hash, str_equal);
    }

    public IoctlBase(MainContext? ctx)
    {
        /* Executed after construct, so don't overwrite the default if not set. */
        if (ctx != null)
            _ctx = ctx;
    }

    ~IoctlBase()
    {
    }

    internal async void socket_listen(SocketListener listener, string devnode)
    {
        Cancellable cancellable;

        lock (listeners)
          cancellable = listeners[devnode];

        try {
            while (true) {
                SocketConnection connection;
                IoctlClient client;

                connection = yield listener.accept_async(cancellable);

                client = new IoctlClient(this, connection, devnode);

                client_connected(client);
                client.read_ioctl.begin();
            }
        } catch (GLib.Error e) {
            if (!(e is IOError.CANCELLED))
                error("Could not accept new connection: %s", e.message);
        }

        lock (listeners)
          listeners.remove(devnode);
    }

    internal void register_path(string devnode, string sockpath)
    {
        assert(DirUtils.create_with_parents(Path.get_dirname(sockpath), 0755) == 0);

        Cancellable cancellable = new Cancellable();

        /* We create new listener for each file; purely because we may not
         * have the correct main context in construct yet. */
        SocketListener listener;
        SocketAddress addr;

        listener = new SocketListener();
        addr = new UnixSocketAddress(sockpath);
        try {
            listener.add_address(addr, SocketType.STREAM, SocketProtocol.DEFAULT, this, null);
        } catch (GLib.Error e) {
            warning("Error listening on ioctl socket for %s", devnode);
            return;
        }

        lock (listeners)
          listeners.insert(devnode, cancellable);

        StartListenClosure tmp = new StartListenClosure(this, listener, devnode);
        ctx.invoke(tmp.cb);
    }

    internal void unregister_path(string devnode)
    {
        lock (listeners)
          listeners[devnode].cancel();
    }

    public virtual signal void client_connected(IoctlClient client) {
    }

    public virtual signal void client_vanished(IoctlClient client) {
    }

    /* Not a normal signal because we need the accumulator. */
    public virtual bool handle_ioctl(IoctlClient client) {
        return false;
    }
}

/* Mirror of ioctl_tree.c usbdevfs_reapurb_execute submit_urb static variable */
private IoctlData? last_submit_urb = null;

internal class IoctlTreeHandler : IoctlBase {

    private IoctlTree.Tree tree;

    public IoctlTreeHandler(MainContext? ctx, string file)
    {
        base (ctx);

        Posix.FILE f = Posix.FILE.open(file, "r");
        tree = new IoctlTree.Tree(f);
    }

    public override bool handle_ioctl(IoctlClient client) {
        void* last = null;
        IoctlData? data = null;
        ulong request = client.request;
        ulong size = IoctlTree.data_size_by_id(request);
        ulong type = (request >> Ioctl._IOC_TYPESHIFT) & ((1 << Ioctl._IOC_TYPEBITS) - 1);
        int ret = -1;
        int my_errno;

        if (tree == null) {
            debug("Aborting client because ioctl tree for %s is empty", client.devnode);
            client.abort();
            return true;
        }

        try {
            if (size > 0)
                data = client.arg.resolve(0, size, true, true);

            /* NOTE: The C code assumes pointers are resolved, as such,
             * all non-trivial structures need to be explicitly listed here.
             */
            if (data != null) {
                if (request == Ioctl.USBDEVFS_SUBMITURB) {
                    Ioctl.usbdevfs_urb *urb = (Ioctl.usbdevfs_urb*) data.data;

                    size_t offset = (ulong) &urb.buffer - (ulong) urb;

                    data.resolve(offset, urb.buffer_length, true, false);
                }
            }
        } catch (IOError e) {
            warning("Error resolving IOCtl data: %s", e.message);
            return false;
        }

        last = client.get_data("last");

        if ((char) type == 'E') {
            Posix.errno = Posix.ENOENT;
        } else {
            Posix.errno = Posix.ENOTTY;
        }
        last = tree.execute(last, request, *(void**) client.arg.data, ref ret);
        my_errno = Posix.errno;
        Posix.errno = 0;
        if (last != null)
            client.set_data("last", last);

        if (ret != -1) {
            my_errno = 0;
        }

        if (request == Ioctl.USBDEVFS_SUBMITURB && ret == 0) {
            last_submit_urb = data;
        }

        if ((request == Ioctl.USBDEVFS_REAPURB || request == Ioctl.USBDEVFS_REAPURBNDELAY) && last_submit_urb != null) {
            /* Parameter points to a pointer, check whether that is a pointer
             * to our last submit urb. If so, update it so the client sees
             * the right information.
             * This should only happen for REAPURB, but it does not hurt to
             * just always check.
             */
            if (*(void**) data.data == (void*) last_submit_urb.data) {
                try {
                    data.set_ptr(0, last_submit_urb);
                } catch (IOError e) {
                    return false;
                }

                Ioctl.usbdevfs_urb *urb = (Ioctl.usbdevfs_urb*) last_submit_urb.data;

                /* Note that the client side buffer may be de-allocated at this point.
                 * As such, we only mark dirty recursively (i.e. including the buffer)
                 * if this was an input EP */
                if ((urb.endpoint & 0x80) == 0x80)
                    last_submit_urb.dirty(true);
                else
                    last_submit_urb.dirty(false);

                last_submit_urb = null;
            }
        }

        client.complete(ret, my_errno);

        return true;
    }
}

}
