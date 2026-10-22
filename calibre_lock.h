#pragma once

// Calibre's own lock on its libraries: while Cousoir holds it, the Calibre
// GUI, calibredb and the content server refuse to start on this computer, and
// it cannot be taken while one of them runs. Released when destroyed, and by
// the system if Cousoir crashes.
class CalibreLock
{
public:
    CalibreLock() = default;
    ~CalibreLock();
    CalibreLock(const CalibreLock &) = delete;
    CalibreLock &operator=(const CalibreLock &) = delete;

    // False when a Calibre program holds it. Always true where Cousoir
    // cannot take it: Calibre locks with a mutex on Windows and a lock file
    // on macOS, which Cousoir does not check yet.
    bool acquire();
    void release();
    // Whether this process holds the lock: the lock seen from outside is
    // then Cousoir's own.
    static bool heldHere();

private:
    int m_socket = -1;
};
