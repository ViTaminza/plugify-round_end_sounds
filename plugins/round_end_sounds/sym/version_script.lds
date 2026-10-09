{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* permissions binding slots (optional dependency) */
        __permissions_*;

        /* hudmenu binding slots - pisexcompat resolves every displayed menu through
           __hudmenu_GetClientMenuType when the hudmenu plugin is installed */
        __hudmenu_*;

        /* No exported methods: round_end_sounds exposes nothing to other plugins. */

    local: *;
};
