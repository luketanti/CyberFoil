# libhaze

libhaze is [haze](https://github.com/Atmosphere-NX/Atmosphere/tree/master/troposphere/haze) made into a easy to use library.

---

## changes

some changes to haze were made:

- instead of [allocating the entire heap](https://github.com/Atmosphere-NX/Atmosphere/blob/8b88351cb46afab3907e395f40733854fd8de0cf/troposphere/haze/source/ptp_object_heap.cpp#L45), libhaze allocates 2x 20Mib blocks.
- console_main_loop.hpp was changed to accept a stop_token to allow signalling for exit from another thread.
- console_main_loop.hpp was changed to remove console gfx code.
- `SuspendAndWaitForFocus()` loop now sleeps thread instead of spinlooping until focus state changes.
- added event callback for when files are created, deleted, written and read.
- add support for custom mount points, rather than just mounting sdmc.

---

## Credits

All credit for libhaze goes to [liamwhite](https://github.com/liamwhite) and the [Atmosphere team](https://github.com/Atmosphere-NX/Atmosphere).
