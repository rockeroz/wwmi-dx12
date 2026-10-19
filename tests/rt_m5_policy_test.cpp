#include "../reshade_addon/rt/rt_m5_policy.h"
#include <cstdlib>
#include <iostream>
#include <memory>

static void Check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main()
{
    using namespace rtcritical;
    Check(ReservationCapacity(250080) == 65536, "tail is additional capacity");
    Check(ReservationCapacity(1000000) == 0, "cannot expand past the descriptor limit");
    Check(ReservationCapacity(UINT32_MAX) == 0, "overflow is refused");
    Check(ReservationCapacity(100) == 0, "small helper heaps stay unchanged");
    FreeRanges pool(250080, 120);
    uint32_t a, b, c;
    Check(pool.Take(40, &a) && a == 250080, "application slots cannot be allocated");
    Check(pool.Take(40, &b) && b == a + 40, "live ranges do not overlap");
    Check(pool.Take(40, &c) && c == b + 40, "last reserved range fits");
    uint32_t ignored = 0;
    Check(!pool.Take(1, &ignored), "exhaustion refuses instead of borrowing application slots");
    pool.Put(b, 40);
    Check(!pool.Take(41, &ignored), "fragmented free ranges are not overstated");
    pool.Put(a, 40);
    Check(pool.Take(80, &ignored) && ignored == a, "adjacent retired ranges coalesce");
    pool.Put(a, 80); pool.Put(c, 40);
    Check(pool.Take(120, &ignored) && ignored == a, "whole tail recovered only after every release");
    Check(!pool.Take(0, &ignored), "empty allocation refused");

    PublicationState normal;
    Check(normal.Commit(true), "first final block publishes");
    Check(!normal.Commit(true) && !normal.Commit(false), "published data cannot be changed");
    PublicationState timeout;
    Check(timeout.Commit(false), "timeout freezes the fallback");
    Check(!timeout.Commit(true) && timeout.Get() == PublicationState::fallback, "late refill cannot overwrite a running copy");
    std::cout << "rt_m5_policy_test: PASS\n";
}
