
namespace UMockdev {

/**
 * SECTION:umockdev-ioctl
 * @title: umockdev-ioctl
 * @short_description: Emulate ioctl and read/write calls for devices.
 *
 * These classes permit emulation of ioctl and read/write calls including
 * fully customizing the behaviour by creating an #UMockdevIoctlBase instance or
 * subclass instance and attaching it using umockdev_testbed_attach_ioctl().
 */

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
 * The #UMockdevIoctlData struct is a container designed to read and write
 * memory from the client process.
 *
 * After memory has been resolved, the corresponding pointer will point to
 * local memory that can be used normally. The memory will automatically be
 * synced back by umockdev_ioctl_client_complete().
 *
 * Since: 0.16
 */
public class IoctlData : GLib.Object {
    /* Local cache to check if data is dirty before flushing. This is both an
     * optimization as it avoids flushes, but is also necessary to avoid
     * writing into read-only memory.
     */
    private uint8[] client_data;

    [CCode(array_length_cname="data_len")]
    public uint8[] data;

    public ulong client_addr;

    private IOStream stream;

    private IoctlData[] children;
    private size_t[] children_offset;

    internal IoctlData(IOStream stream)
    {
        this.stream = stream;
    }

    /**
     * umockdev_ioctl_data_ref:
     * @self: A #UMockdevIoctlData
     *
     * Deprecated, same as g_object_ref().
     */
    [CCode(cname="umockdev_ioctl_data_ref")]
    public new IoctlData? compat_ref()
    {
        return (IoctlData?) this;
    }

    /**
     * umockdev_ioctl_data_unref:
     * @self: A #UMockdevIoctlData
     *
     * Deprecated, same as g_object_unref().
     */
    [CCode(cname="umockdev_ioctl_data_unref")]
    public new void compat_unref()
    {
        this.unref();
    }

    /**
     * umockdev_ioctl_data_resolve:
     * @self: A #UMockdevIoctlData
     * @offset: Byte offset of pointer inside data
     * @len: Length of the to be resolved data
     * @error: return location for a GError, or %NULL
     *
     * Resolve an address inside the data. After this operation, the pointer
     * inside data points to a local copy of the memory. Any local modifications
     * will be synced back by umockdev_ioctl_client_complete() and
     * umockdev_ioctl_client_execute().
     *
     * You may call this multiple times on the same pointer in order to fetch
     * the existing information.
     *
     * Returns: #UMockdevIoctlData, or #NULL on error
     * Since: 0.16
     */
    public IoctlData? resolve(size_t offset, size_t len) throws IOError {
        IoctlData res;

        for (int i = 0; i < children.length; i++) {
            if (children_offset[i] == offset)
                return children[i];
        }

        if (offset + sizeof(size_t) > data.length)
            return null;

        res = new IoctlData(stream);
        res.data = new uint8[len];
        res.client_addr = *((size_t*) &data[offset]);

        children += res;
        children_offset += offset;

        /* Don't try to resolve null pointers. */
        if (res.client_addr == 0 || len == 0)
            return null;

        *((size_t*) &data[offset]) = (size_t) res.data;

        res.load_data();

        return res;
    }

    /**
     * umockdev_ioctl_data_set_ptr:
     * @self: A #UMockdevIoctlData
     * @offset: Byte offset of pointer inside data
     * @child: Memory block that the pointer should point to
     * @error: return location for a GError, or %NULL
     *
     * Basically the reverse operation of umockdev_ioctl_data_resolve(). It sets
     * the pointer at @offset to point to the data from @child in a way that
     * can be synchronised back to the client.
     *
     * Use of this is rare, but e.g. required to reap USB URBs.
     *
     * The function will only work correctly for pointer elements that have not
     * been resolved before.
     *
     * Returns: %TRUE on success, %FALSE
     * Since: 0.16
     */
    public bool set_ptr(size_t offset, IoctlData child) {

        foreach (size_t o in children_offset) {
            assert(o != offset);
        }

        assert(offset + sizeof(size_t) <= data.length);

        children += child;
        children_offset += offset;

        *((size_t*) &data[offset]) = (size_t) child.data;

        return true;
    }

    /**
     * umockdev_ioctl_data_reload:
     * @self: A #UMockdevIoctlData
     * @error: return location for a GError, or %NULL
     *
     * This function allows reloading the data from the client side in case
     * you expect client modifications to have happened in the meantime (e.g.
     * between two separate ioctl's).
     * It is very unlikely that such an explicit reload is needed.
     *
     * Doing this unresolves any resolved pointers. Take care to re-resolve
     * them and use the newly resolved #UMockdevIoctlData in case you need to
     * access the data.
     *
     * Returns: #TRUE on success, #FALSE otherwise
     * Since: 0.16
     */
    public bool reload() throws IOError {
        load_data();

        children.resize(0);
        children_offset.resize(0);

        return true;
    }

    /**
     * umockdev_ioctl_update:
     * @self: A #UMockdevIoctlData
     * @offset: Offset into data
     * @new_data: (array length=length): Data to set
     * @new_data_length1: Lenght of binary data, must be smaller or equal to actual length
     *
     * Set data to a specific value. This is essentially a memcpy call, it is
     * only useful for e.g. python where the bindings cannot make the data
     * writable otherwise.
     *
     * Since: 0.18
     */
    public void update(size_t offset, uint8[] new_data) {
        assert(offset + new_data.length <= data.length);

        Posix.memcpy(&data[offset], new_data, new_data.length);
    }

    /**
     * umockdev_ioctl_retrieve:
     * @self: A #UMockdevIoctlData
     * @read_data: (array length=length) (out): Data to set
     * @read_data_length1: (out): Lenght of binary data, must be smaller or equal to actual length
     *
     * Simply returns the data struct member. This function purely exists for
     * GIR based bindings, as the vala generated bindings do not correctly
     * tag the array length, and direct access to the struct member is not
     * possible.
     *
     * Since: 0.18
     */
    public void retrieve(out uint8[] read_data) {
        read_data = data;
    }

    internal void load_data() throws IOError {
        OutputStream output = stream.get_output_stream();
        InputStream input = stream.get_input_stream();
        ulong args[3];

        /* The original argument has no memory associated to it */
        if (client_addr == 0)
            return;

        client_data = new uint8[data.length];

        args[0] = 5; /* READ_MEM */
        args[1] = client_addr;
        args[2] = data.length;

        output.write_all((uint8[])args, null, null);
        input.read_all(client_data, null, null);

        Posix.memcpy(data, client_data, data.length);
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

        if (client_addr != 0 &&
            submit_data.length == client_data.length &&
            Posix.memcmp(submit_data, client_data, submit_data.length) != 0) {

            OutputStream output = stream.get_output_stream();
            ulong args[3];

            args[0] = 6; /* WRITE_MEM */
            args[1] = client_addr;
            args[2] = submit_data.length;

            yield output.write_all_async((uint8[])args, 0, null, null);
            yield output.write_all_async(submit_data, 0, null, null);
        }
    }

    internal void flush_sync() throws IOError {
        uint8[] submit_data = data;

        for (int i = 0; i < children.length; i++) {
            children[i].flush_sync();

            *((ulong*) &submit_data[children_offset[i]]) = children[i].client_addr;
        }

        if (client_addr != 0 &&
            submit_data.length == client_data.length &&
            Posix.memcmp(submit_data, client_data, submit_data.length) != 0) {

            OutputStream output = stream.get_output_stream();
            ulong args[3];

            args[0] = 6; /* WRITE_MEM */
            args[1] = client_addr;
            args[2] = submit_data.length;

            output.write_all((uint8[])args, null, null);
            output.write_all(submit_data, null, null);
        }
    }
}

/**
 * UMockdevIoctlClient:
 *
 * The #UMockdevIoctlClient struct represents an opened client side FD in order
 * to emulate ioctl calls on this device.
 *
 * Since: 0.16
 */
/**
 * UMockdevIoctlClient::handle-ioctl:
 * @client: A #UMockdevIoctlClient
 *
 * Called when an ioctl is requested by the client.
 *
 * This is the per-client signal. See #UMockdevIoctlBase::handle-ioctl on #UMockdevIoctlBase.
 *
 * Since: 0.16
 */
/**
 * UMockdevIoctlClient::handle-read:
 * @client: A #UMockdevIoctlClient
 *
 * Called when a read is requested by the client.
 *
 * This is the per-client signal. See #UMockdevIoctlBase::handle-read on #UMockdevIoctlBase.
 *
 * Since: 0.16
 */
/**
 * UMockdevIoctlClient::handle-write:
 * @client: A #UMockdevIoctlClient
 *
 * Called when a write is requested by the client.
 *
 * This is the per-client signal. See #UMockdevIoctlBase::handle-write on #UMockdevIoctlBase.
 *
 * Since: 0.16
 */

public class IoctlClient : GLib.Object {
    private IoctlBase handler;
    private IOStream stream;
    private GLib.MainContext _ctx;

    [Description(nick = "device node", blurb = "The device node the client opened")]
    public string devnode { get; }

    [Description(nick = "request", blurb = "The current ioctl request")]
    public ulong request { get; }
    [Description(nick = "argument", blurb = "The ioctl argument, for read/write the passed buffer")]
    public IoctlData arg { get; }
    [Description(nick = "connected", blurb = "Whether the client is still connected")]
    public bool connected {
      get {
        return !stream.is_closed();
      }
    }

    private ulong _cmd;
    private bool _abort;
    private long result;
    private int result_errno;

    static construct {
        GLib.Signal.@new("handle-ioctl", typeof(IoctlClient), GLib.SignalFlags.RUN_LAST, 0, signal_accumulator_true_handled, null, null, typeof(bool), 0);
        GLib.Signal.@new("handle-read", typeof(IoctlClient), GLib.SignalFlags.RUN_LAST, 0, signal_accumulator_true_handled, null, null, typeof(bool), 0);
        GLib.Signal.@new("handle-write", typeof(IoctlClient), GLib.SignalFlags.RUN_LAST, 0, signal_accumulator_true_handled, null, null, typeof(bool), 0);
    }

    /**
     * umockdev_ioctl_client_execute:
     * @self: A #UMockdevIoctlClient
     * @errno_: Return location for errno
     * @error: return location for a GError, or %NULL
     *
     * This function is not generally useful. It exists for umockdev itself
     * in order to implement recording.
     *
     * Execute the ioctl on the client side. Note that this flushes any
     * modifications of the ioctl data. As such, pointers that were already
     * resolved (including the initial ioctl argument itself) need to be
     * resolved again.
     *
     * It is only valid to call this while an uncompleted command is being
     * processed.
     *
     * This call is thread-safe.
     *
     * Returns: The client side result of the ioctl call
     *
     * Since: 0.16
     */
    public int execute(out int errno_) throws IOError {
        OutputStream output = stream.get_output_stream();
        InputStream input = stream.get_input_stream();
        ulong args[3];

        assert(_cmd != 0);

        arg.flush_sync();

        args[0] = 4; /* RUN (original ioctl/read/write) */
        args[1] = 0; /* unused */
        args[2] = 0; /* unused */

        output.write_all((uint8[])args, null, null);
        input.read_all((uint8[])args, null, null);

        assert(args[0] == 2); /* RES (result) */

        /* Reload data, will usually not do anything for ioctl's as nothing
         * will have been resolved. */
        _arg.reload();

        errno_ = (int) args[2];

        return (int) args[1];
    }

    /**
     * umockdev_ioctl_client_complete:
     * @self: A #UMockdevIoctlClient
     * @res: Return value of ioctl
     * @errno_: errno of ioctl
     *
     * Asynchronously completes the ioctl invocation of the client. This is
     * equivalent to calling umockdev_ioctl_client_complete() with the
     * invocation.
     *
     * This call is thread-safe.
     *
     * Since: 0.16
     */
    public void complete(long res, int errno_) {
        /* Nullify some of the request information */
        assert(_cmd != 0);
        _cmd = 0;
        _request = 0;

        this.result = res;
        this.result_errno = errno_;

        /* Push us into the correct main context. */
        _ctx.invoke(complete_idle);
    }

    /**
     * umockdev_ioctl_client_abort:
     * @self: A #UMockdevIoctlClient
     *
     * Asynchronously terminates the child by asking it to execute exit(1).
     *
     * This call is thread-safe.
     *
     * Since: 0.16
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
        _cmd = 0;
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

        assert(args[0] == 1 || args[0] == 7 || args[0] == 8);
        _cmd = args[0];

        if (args[0] == 1) {
            _request = args[1];
            _arg = new IoctlData(stream);
            _arg.data = new uint8[sizeof(ulong)];
            *(ulong*) _arg.data = args[2];
        } else {
            _request = 0;
            _arg = new IoctlData(stream);
            _arg.data = new uint8[args[2]];
            _arg.client_addr = args[1];

            try {
                _arg.load_data();
            } catch (IOError e) {
                warning("Error resolving IOCtl data: %s", e.message);
                complete(-100, 0);
                return;
            }
        }

        bool handled = false;

        if (args[0] == 1)
            GLib.Signal.emit_by_name(this, "handle-ioctl", out handled);
        else if (args[0] == 7)
            GLib.Signal.emit_by_name(this, "handle-read", out handled);
        else
            GLib.Signal.emit_by_name(this, "handle-write", out handled);

        if (!handled) {
            if (args[0] == 1)
                GLib.Signal.emit_by_name(handler, "handle-ioctl", this, out handled);
            else if (args[0] == 7)
                GLib.Signal.emit_by_name(handler, "handle-read", this, out handled);
            else
                GLib.Signal.emit_by_name(handler, "handle-write", this, out handled);
        }

        if (!handled && args[0] == 1) {
            /* No specific handler for this ioctl. First try stateless ioctls
             * (like USB ioctls), then fall back to executing on the real fd
             * for terminal ioctls on PTY-backed devices. */
            IoctlTree.Tree tree = null;
            IoctlData? data = null;
            ulong size = IoctlTree.data_size_by_id(_request);
            ulong type = (_request >> Ioctl._IOC_TYPESHIFT) & ((1 << Ioctl._IOC_TYPEBITS) - 1);
            int ret;
            int my_errno;

            try {
                if (size > 0)
                    data = _arg.resolve(0, size);
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
            tree.execute(null, _request, *(void**) _arg.data, out ret);
            my_errno = Posix.errno;

            /* For termios ioctls (TCGETS, etc.), try executing on the real fd (PTY). */
            if (ret == -1 && my_errno == Posix.ENOTTY && IoctlTermios.is_termios_ioctl(_request)) {
                try {
                    ret = execute(out my_errno);
                    /* execute() returns errno from real ioctl, which preserves errno on success */
                } catch (IOError e) {
                    /* If execute() throws, keep ENOTTY */
                }
            } else if (ret != -1) {
                my_errno = 0;
            }

            complete(ret, my_errno);
        } else if (!handled) {
            complete(-100, 0);
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
        _ctx.invoke(notify_closed_idle);
    }

    internal IoctlClient(IoctlBase handler, IOStream stream, string devnode)
    {
        this.handler = handler;
        this.stream = stream;
        this._devnode = devnode;
        this._ctx = GLib.MainContext.get_thread_default();

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
 * UMockdevIoctlBaseClass:
 * @handle_ioctl: Override ioctl emulation
 * @handle_read: Override read emulation
 * @handle_write: Override write_emulation
 * @client_connected: A device was opened
 * @client_vanished: A device was closed
 *
 * The base class for an device ioctl and read/write handling. You can either
 * override the corresponding vfuncs or connect to the signals to customize
 * the emulation.
 *
 * Since: 0.16
 */

/**
 * UMockdevIoctlBase:
 *
 * The #UMockdevIoctlBase class is a base class to emulate and record ioctl
 * operations of a client. It can be attached to an emulated device in the
 * testbed and will then be used.
 *
 * Since: 0.16
 */
/**
 * UMockdevIoctlBase::handle-ioctl:
 * @handler: A #UMockdevIoctlBase
 * @client: A #UMockdevIoctlClient
 *
 * Called when an ioctl is requested by the client.
 *
 * Access the #UMockdevIoctlClient:arg property of @client to retrieve the
 * argument of the ioctl. This is a pointer sized buffer initially with the
 * original argument passed to the ioctl. If this is pointing to a struct, use
 * umockdev_ioctl_data_resolve() to retrieve the underlying memory and update
 * the pointer. Resolve any further pointers in the structure in the same way.
 *
 * After resolving the memory, you can access it as if it was local. The memory
 * will be synced back to the client automatically if it has been modified
 * locally.
 *
 * Once processing is done, use umockdev_ioctl_client_complete() to let the
 * client continue with the result of the emulation. You can also use
 * umockdev_ioctl_client_abort() to kill the client. Note that this handling
 * does not need to be immediate. It is valid to immediately return #TRUE from
 * this function and call umockdev_ioctl_client_complete() at a later point.
 *
 * Note that this function will be called from a worker thread with a private
 * #GMainContext for the #UMockdevTestbed. Do not block this context for longer
 * periods. The complete handler may be called from a different thread.
 *
 * Returns: #TRUE if the request is being handled, #FALSE otherwise.
 * Since: 0.16
 */
/**
 * UMockdevIoctlBase::handle-read:
 * @handler: A #UMockdevIoctlBase
 * @client: A #UMockdevIoctlClient
 *
 * Called when a read is requested by the client.
 *
 * The result buffer is represented by #UMockdevIoctlClient:arg of @client.
 * Retrieve its length to find out the requested read length. The content of
 * the buffer has already been retrieved, and you can freely use and update it.
 *
 * See #UMockdevIoctlBase::handle-ioctl for some more information.
 *
 * Returns: #TRUE if the request is being handled, #FALSE otherwise.
 * Since: 0.16
 */
/**
 * UMockdevIoctlBase::handle-write:
 * @handler: A #UMockdevIoctlBase
 * @client: A #UMockdevIoctlClient
 *
 * Called when a write is requested by the client.
 *
 * The written buffer is represented by #UMockdevIoctlClient:arg of @client.
 * Retrieve its length to find out the requested write length. The content of
 * the buffer has already been retrieved, and you can freely use it.
 *
 * See #UMockdevIoctlBase::handle-ioctl for some more information.
 *
 * Returns: #TRUE if the request is being handled, #FALSE otherwise.
 * Since: 0.16
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


[ CCode(cname="G_STRUCT_OFFSET(UMockdevIoctlBaseClass, handle_ioctl)") ]
extern const int IOCTL_BASE_HANDLE_IOCTL_OFFSET;
[ CCode(cname="G_STRUCT_OFFSET(UMockdevIoctlBaseClass, handle_read)") ]
extern const int IOCTL_BASE_HANDLE_READ_OFFSET;
[ CCode(cname="G_STRUCT_OFFSET(UMockdevIoctlBaseClass, handle_write)") ]
extern const int IOCTL_BASE_HANDLE_WRITE_OFFSET;

public class IoctlBase: GLib.Object {
    private HashTable<string,Cancellable> listeners;

    static construct {
        GLib.Signal.@new("handle-ioctl", typeof(IoctlBase), GLib.SignalFlags.RUN_LAST, IOCTL_BASE_HANDLE_IOCTL_OFFSET, signal_accumulator_true_handled, null, null, typeof(bool), 1, typeof(IoctlClient));
        GLib.Signal.@new("handle-read", typeof(IoctlBase), GLib.SignalFlags.RUN_LAST, IOCTL_BASE_HANDLE_READ_OFFSET, signal_accumulator_true_handled, null, null, typeof(bool), 1, typeof(IoctlClient));
        GLib.Signal.@new("handle-write", typeof(IoctlBase), GLib.SignalFlags.RUN_LAST, IOCTL_BASE_HANDLE_WRITE_OFFSET, signal_accumulator_true_handled, null, null, typeof(bool), 1, typeof(IoctlClient));
    }

    construct {
        listeners = new HashTable<string,Cancellable>(str_hash, str_equal);
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

        listener.close();
    }

#if INTERNAL_REGISTER_API
    internal void register_path(GLib.MainContext? ctx, string devnode, string sockpath)
    {
        assert(DirUtils.create_with_parents(Path.get_dirname(sockpath), 0755) == 0);

        Cancellable cancellable = new Cancellable();

        cancellable.set_data("sockpath", sockpath);

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

#if INTERNAL_UNREGISTER_PATH_API
    internal void unregister_path(string devnode)
    {
        lock (listeners) {
            listeners[devnode].cancel();
            Posix.unlink(listeners[devnode].get_data("sockpath"));
            listeners.remove(devnode);
        }
    }
#endif

#if INTERNAL_UNREGISTER_ALL_API
    internal void unregister_all()
    {
        lock (listeners) {
            listeners.foreach_remove((key, val) => {
                val.cancel();
                Posix.unlink(val.get_data("sockpath"));
                return true;
            });
        }
    }
#endif

#endif // INTERNAL_REGISTER_API

    /* Not a normal signal because we need the accumulator. */
    public virtual bool handle_ioctl(IoctlClient client) {
        return false;
    }

    public virtual bool handle_read(IoctlClient client) {
        return false;
    }

    public virtual bool handle_write(IoctlClient client) {
        return false;
    }

    public virtual signal void client_connected(IoctlClient client) {
    }

    public virtual signal void client_vanished(IoctlClient client) {
    }
}

/* Mirror of ioctl_tree.c usbdevfs_reapurb_execute submit_urb static variable */
private IoctlData? last_submit_urb = null;

internal class IoctlTreeHandler : IoctlBase {

    private IoctlTree.Tree tree;

    public IoctlTreeHandler(string file)
    {
        base ();

        Posix.FILE f = Posix.FILE.open(file, "r");
        tree = new IoctlTree.Tree(f);
    }

    public override bool handle_ioctl(IoctlClient client) {
        void* last = null;
        IoctlData? data = null;
        ulong request = client.request;
        ulong size = IoctlTree.data_size_by_id(request);
        ulong type = (request >> Ioctl._IOC_TYPESHIFT) & ((1 << Ioctl._IOC_TYPEBITS) - 1);
        int ret;
        int my_errno;

        if (tree == null) {
            debug("Aborting client because ioctl tree for %s is empty", client.devnode);
            client.abort();
            return true;
        }

        last = client.get_data("last");

        try {
            if (request == Ioctl.CROS_EC_DEV_IOCXCMD_V2) {
                size += tree.next_ret(last);
            }

            if (size > 0)
                data = client.arg.resolve(0, size);

            /* NOTE: The C code assumes pointers are resolved, as such,
             * all non-trivial structures need to be explicitly listed here.
             */
            if (data != null) {
                if (request == Ioctl.USBDEVFS_SUBMITURB) {
                    Ioctl.usbdevfs_urb *urb = (Ioctl.usbdevfs_urb*) data.data;

                    size_t offset = (ulong) &urb.buffer - (ulong) urb;

                    data.resolve(offset, urb.buffer_length);
                }
            }
        } catch (IOError e) {
            warning("Error resolving IOCtl data: %s", e.message);
            return false;
        }

        if ((char) type == 'E') {
            Posix.errno = Posix.ENOENT;
        } else {
            Posix.errno = Posix.ENOTTY;
        }
        last = tree.execute(last, request, *(void**) client.arg.data, out ret);
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
                data.set_ptr(0, last_submit_urb);

                last_submit_urb = null;
            }
        }

        client.complete(ret, my_errno);

        return true;
    }
}

internal class IoctlTreeRecorder : IoctlBase {

    bool write_log;
    string logfile;
    string device;
    private IoctlTree.Tree tree;

    public IoctlTreeRecorder(string device, string file)
    {
        string existing_device_path = null;
        Posix.FILE log;
        base ();

        this.logfile = file;
        this.device = device;
        log = Posix.FILE.open(logfile, "r");
        if (log == null)
            return;

        /* Check @DEV header and parse log */
        if (log.scanf("@DEV %ms\n", &existing_device_path) == 1 && existing_device_path != device)
            error("attempt to record two different devices to the same ioctl recording");
        tree = new IoctlTree.Tree(log);
    }

    ~IoctlTreeRecorder()
    {
        flush_log();
    }

    private void flush_log() {
        Posix.FILE log;

        /* Only write log file if we ever saw a client. */
        if (!write_log)
            return;

        assert (device != null);

        log = Posix.FILE.open(logfile, "w+");
        log.printf("@DEV %s\n", device);
        tree.write(log);
    }

    public override bool handle_ioctl(IoctlClient client) {
        ulong request = client.request;
        ulong size = IoctlTree.data_size_by_id(request);
        IoctlData data = null;
        IoctlTree.Tree node;
        int ret;
        int my_errno;

        try {
            /* Execute real ioctl */
            ret = client.execute(out my_errno);

            /* We do not record errors with the ioctl tree driver. */
            if (ret == -1) {
                client.complete(ret, my_errno);
                return true;
            }

            if (request == Ioctl.CROS_EC_DEV_IOCXCMD_V2) {
              size += ret;
            }

            /* Resolve data */
            if (size > 0)
                data = client.arg.resolve(0, size);

            /* NOTE: The C code assumes pointers are resolved, as such,
             * all non-trivial structures need to be explicitly listed here.
             */
            if (data != null) {
                IoctlData urb_data = null;

                if (request == Ioctl.USBDEVFS_REAPURB ||
                    request == Ioctl.USBDEVFS_REAPURBNDELAY) {

                    urb_data = data.resolve(0, sizeof(Ioctl.usbdevfs_urb));
                } else if (request == Ioctl.USBDEVFS_SUBMITURB) {
                    urb_data = data;
                }

                if (urb_data != null) {
                    Ioctl.usbdevfs_urb *urb = (Ioctl.usbdevfs_urb*) urb_data.data;

                    size_t offset = (ulong) &urb.buffer - (ulong) urb;

                    urb_data.resolve(offset, urb.buffer_length);
                }
            }
        } catch (IOError e) {
            warning("Error executing and recording ioctl: %s", e.message);
            return false;
        }

        /* Record */
        node = new IoctlTree.Tree.from_bin(request, *(void**) client.arg.data, ret);
        if (node != null) {
            tree.insert((owned) node);
        }

        /* Let client continue */
        client.complete(ret, my_errno);

        return true;
    }

    public override void client_connected(IoctlClient client) {
        write_log = true;
    }
}

}
