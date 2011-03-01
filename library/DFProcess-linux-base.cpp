/*
www.sourceforge.net/projects/dfhack
Copyright (c) 2009 Petr Mrázek (peterix), Kenneth Ferland (Impaler[WrG]), dorf

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/
#include "Internal.h"
#include "LinuxProcess.h"
#include "dfhack/VersionInfo.h"
#include "dfhack/DFError.h"
#include <errno.h>
#include <sys/ptrace.h>
using namespace DFHack;

LinuxProcessBase::LinuxProcessBase(uint32_t pid)
: my_pid(pid)
{
    my_descriptor = NULL;
    attached = false;
    suspended = false;
    memFileHandle = 0;
}

bool LinuxProcessBase::isSuspended()
{
    return suspended;
}
bool LinuxProcessBase::isAttached()
{
    return attached;
}

bool LinuxProcessBase::isIdentified()
{
    return identified;
}

LinuxProcessBase::~LinuxProcessBase()
{
    if(attached)
    {
        detach();
    }
    // destroy our copy of the memory descriptor
    if(my_descriptor)
        delete my_descriptor;
}

VersionInfo * LinuxProcessBase::getDescriptor()
{
    return my_descriptor;
}

int LinuxProcessBase::getPID()
{
    return my_pid;
}

//FIXME: implement
bool LinuxProcessBase::getThreadIDs(vector<uint32_t> & threads )
{
    return false;
}

//FIXME: cross-reference with ELF segment entries?
void LinuxProcessBase::getMemRanges( vector<t_memrange> & ranges )
{
    char buffer[1024];
    char permissions[5]; // r/-, w/-, x/-, p/s, 0

    sprintf(buffer, "/proc/%lu/maps", (long unsigned)my_pid);
    FILE *mapFile = ::fopen(buffer, "r");
    uint64_t offset, device1, device2, node;

    while (fgets(buffer, 1024, mapFile))
    {
        t_memrange temp;
        temp.name[0] = 0;
        sscanf(buffer, "%zx-%zx %s %zx %2zu:%2zu %zu %s",
               &temp.start,
               &temp.end,
               (char*)&permissions,
               &offset, &device1, &device2, &node,
               (char*)&temp.name);
        temp.read = permissions[0] == 'r';
        temp.write = permissions[1] == 'w';
        temp.execute = permissions[2] == 'x';
        temp.valid = true;
        ranges.push_back(temp);
    }
}

bool LinuxProcessBase::asyncSuspend()
{
    return suspend();
}

bool LinuxProcessBase::suspend()
{
    int status;
    if(!attached)
        return false;
    if(suspended)
        return true;
    if (kill(my_pid, SIGSTOP) == -1)
    {
        // no, we got an error
        perror("kill SIGSTOP error");
        return false;
    }
    while(true)
    {
        // we wait on the pid
        pid_t w = waitpid(my_pid, &status, 0);
        if (w == -1)
        {
            // child died
            perror("DF exited during suspend call");
            return false;
        }
        // stopped -> let's continue
        if (WIFSTOPPED(status))
        {
            break;
        }
    }
    suspended = true;
    return true;
}

bool LinuxProcessBase::forceresume()
{
    return resume();
}

bool LinuxProcessBase::resume()
{
    if(!attached)
        return false;
    if(!suspended)
        return true;
    if (ptrace(PTRACE_CONT, my_pid, NULL, NULL) == -1)
    {
        // no, we got an error
        perror("ptrace resume error");
        return false;
    }
    suspended = false;
    return true;
}


bool LinuxProcessBase::attach()
{
    int status;
    if(attached)
    {
        if(!suspended)
            return suspend();
        return true;
    }
    // can we attach?
    if (ptrace(PTRACE_ATTACH , my_pid, NULL, NULL) == -1)
    {
        // no, we got an error
        perror("ptrace attach error");
        cerr << "attach failed on pid " << my_pid << endl;
        return false;
    }
    while(true)
    {
        // we wait on the pid
        pid_t w = waitpid(my_pid, &status, 0);
        if (w == -1)
        {
            // child died
            perror("wait inside attach()");
            return false;
        }
        // stopped -> let's continue
        if (WIFSTOPPED(status))
        {
            break;
        }
    }
    suspended = true;

    int proc_pid_mem = open(memFile.c_str(),O_RDONLY);
    if(proc_pid_mem == -1)
    {
        ptrace(PTRACE_DETACH, my_pid, NULL, NULL);
        cerr << memFile << endl;
        cerr << "couldn't open /proc/" << my_pid << "/mem" << endl;
        perror("open(memFile.c_str(),O_RDONLY)");
        return false;
    }
    else
    {
        attached = true;

        memFileHandle = proc_pid_mem;
        return true; // we are attached
    }
}

bool LinuxProcessBase::detach()
{
    if(!attached) return true;
    if(!suspended) suspend();
    int result = 0;
    // close /proc/PID/mem
    result = close(memFileHandle);
    if(result == -1)
    {
        cerr << "couldn't close /proc/"<< my_pid <<"/mem" << endl;
        perror("mem file close");
        return false;
    }
    else
    {
        // detach
        result = ptrace(PTRACE_DETACH, my_pid, NULL, NULL);
        if(result == -1)
        {
            cerr << "couldn't detach from process pid" << my_pid << endl;
            perror("ptrace detach");
            return false;
        }
        else
        {
            attached = false;
            return true;
        }
    }
}


void LinuxProcessBase::read (const uint32_t offset, const uint32_t size, uint8_t *target)
{
    if(size == 0) return;

    ssize_t result;
    ssize_t total = 0;
    ssize_t remaining = size;
    while (total != size)
    {
        result = pread(memFileHandle, target + total ,remaining,offset + total);
        if(result == -1)
        {
            cerr << "pread failed: can't read " << size << " bytes at addres " << offset << endl;
            cerr << "errno: " << errno << endl;
            errno = 0;
            throw Error::MemoryAccessDenied();
        }
        else
        {
            total += result;
            remaining -= result;
        }
    }
}

void LinuxProcessBase::readByte (const uint32_t offset, uint8_t &val )
{
    read(offset, 1, &val);
}

void LinuxProcessBase::readWord (const uint32_t offset, uint16_t &val)
{
    read(offset, 2, (uint8_t *) &val);
}

void LinuxProcessBase::readDWord (const uint32_t offset, uint32_t &val)
{
    read(offset, 4, (uint8_t *) &val);
}

void LinuxProcessBase::readFloat (const uint32_t offset, float &val)
{
    read(offset, 4, (uint8_t *) &val);
}

void LinuxProcessBase::readQuad (const uint32_t offset, uint64_t &val)
{
    read(offset, 8, (uint8_t *) &val);
}

/*
 * WRITING
 */

void LinuxProcessBase::writeQuad (uint32_t offset, const uint64_t data)
{
    #ifdef HAVE_64_BIT
        ptrace(PTRACE_POKEDATA,my_pid, offset, data);
    #else
        ptrace(PTRACE_POKEDATA,my_pid, offset, (uint32_t) data);
        ptrace(PTRACE_POKEDATA,my_pid, offset+4, (uint32_t) (data >> 32));
    #endif
}

void LinuxProcessBase::writeDWord (uint32_t offset, uint32_t data)
{
    #ifdef HAVE_64_BIT
        uint64_t orig = Process::readQuad(offset);
        orig &= 0xFFFFFFFF00000000;
        orig |= data;
        ptrace(PTRACE_POKEDATA,my_pid, offset, orig);
    #else
        ptrace(PTRACE_POKEDATA,my_pid, offset, data);
    #endif
}

// using these is expensive.
void LinuxProcessBase::writeWord (uint32_t offset, uint16_t data)
{
    #ifdef HAVE_64_BIT
        uint64_t orig = Process::readQuad(offset);
        orig &= 0xFFFFFFFFFFFF0000;
        orig |= data;
        ptrace(PTRACE_POKEDATA,my_pid, offset, orig);
    #else
        uint32_t orig = Process::readDWord(offset);
        orig &= 0xFFFF0000;
        orig |= data;
        ptrace(PTRACE_POKEDATA,my_pid, offset, orig);
    #endif
}

void LinuxProcessBase::writeByte (uint32_t offset, uint8_t data)
{
    #ifdef HAVE_64_BIT
        uint64_t orig = Process::readQuad(offset);
        orig &= 0xFFFFFFFFFFFFFF00;
        orig |= data;
        ptrace(PTRACE_POKEDATA,my_pid, offset, orig);
    #else
        uint32_t orig = Process::readDWord(offset);
        orig &= 0xFFFFFF00;
        orig |= data;
        ptrace(PTRACE_POKEDATA,my_pid, offset, orig);
    #endif
}

// blah. I hate the kernel devs for crippling /proc/PID/mem. THIS IS RIDICULOUS
void LinuxProcessBase::write (uint32_t offset, uint32_t size, uint8_t *source)
{
    uint32_t indexptr = 0;
    while (size > 0)
    {
        #ifdef HAVE_64_BIT
            // quad!
            if(size >= 8)
            {
                writeQuad(offset, *(uint64_t *) (source + indexptr));
                offset +=8;
                indexptr +=8;
                size -=8;
            }
            else
        #endif
        // default: we push 4 bytes
        if(size >= 4)
        {
            writeDWord(offset, *(uint32_t *) (source + indexptr));
            offset +=4;
            indexptr +=4;
            size -=4;
        }
        // last is either three or 2 bytes
        else if(size >= 2)
        {
            writeWord(offset, *(uint16_t *) (source + indexptr));
            offset +=2;
            indexptr +=2;
            size -=2;
        }
        // finishing move
        else if(size == 1)
        {
            writeByte(offset, *(uint8_t *) (source + indexptr));
            return;
        }
    }
}

const std::string LinuxProcessBase::readCString (uint32_t offset)
{
    std::string temp;
    char temp_c[256];
    int counter = 0;
    char r;
    do
    {
        r = Process::readByte(offset+counter);
        temp_c[counter] = r;
        counter++;
    } while (r && counter < 255);
    temp_c[counter] = 0;
    temp = temp_c;
    return temp;
}

string LinuxProcessBase::getPath()
{
    char cwd_name[256];
    char target_name[1024];
    int target_result;

    sprintf(cwd_name,"/proc/%d/cwd", getPID());
    // resolve /proc/PID/exe link
    target_result = readlink(cwd_name, target_name, sizeof(target_name));
    target_name[target_result] = '\0';
    return(string(target_name));
}
