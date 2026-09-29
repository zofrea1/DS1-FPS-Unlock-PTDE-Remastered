#pragma once

// Forwards the XInput exports to the system XINPUT1_3.dll. The PTDE executable
// imports XINPUT1_3 by ordinal (2 and 3), so this is a free injection point that
// does not collide with DSfix, which occupies DINPUT8.dll.
