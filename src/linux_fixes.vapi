[CCode (cprefix = "", lower_case_cprefix = "")]
namespace LinuxFixes {

    [CCode (cprefix = "", lower_case_cprefix = "")]
    namespace Input {

        // https://gitlab.gnome.org/GNOME/vala/-/commit/dbe4b716a8d79704
        [CCode (cname = "struct input_event", has_type_id = false, cheader_filename = "linux/input.h")]
        public struct Event {
            [Version (deprecated = true, replacement = "Event.input_event_sec and Event.input_event_usec")]
            public Posix.timeval time;
            public time_t input_event_sec;
            public long input_event_usec;
            public uint16 type;
            public uint16 code;
            public int32 value;
        }
    }
}
