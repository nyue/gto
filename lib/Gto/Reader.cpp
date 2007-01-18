//******************************************************************************
// Copyright (c) 2003 Tweak Films. 
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2 of
// the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
// USA
//

#include "Reader.h"
#include "Utilities.h"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <assert.h>
#ifdef GTO_SUPPORT_ZIP
#include <zlib.h>
#endif

int GTOParse(Gto::Reader*);

#if ( __GNUC__ == 2 )
    #define IOS_CUR ios::cur
    #define IOS_BEG ios::beg
#else
    #define IOS_CUR ios_base::cur
    #define IOS_BEG ios_base::beg
#endif

namespace Gto {
using namespace std;

Reader::Reader(unsigned int mode) 
    : m_in(0), 
      m_gzfile(0), 
      m_gzrval(0), 
      m_error(false), 
      m_needsClosing(false),
      m_mode(mode),
      m_linenum(0),
      m_charnum(0)
{
}

Reader::~Reader()
{
    close();
}

bool
Reader::open(istream& i, const char *name, unsigned int ormode)
{
    if ((m_in && m_in != &i) || m_gzfile) close();

    m_in            = &i;
    m_needsClosing  = false;
    m_inName        = name;
    m_error         = false;

    if ((m_mode | ormode) & TextOnly)
    {
        return readTextGTO();
    }
    else
    {
        readMagicNumber();

        if (m_header.magic != Header::Magic ||
            m_header.magic == Header::Cigam)
        {
            return false;
        }

        return readBinaryGTO();
    }
}

bool
Reader::open(const char *filename)
{
    if (m_in) return false;
    
    struct stat buf;
    if (stat( filename, &buf ) )
    {
        fail( "File does not exist" );
        return false;
    }

    m_inName = filename;

    //
    //  Fail if not compiled with zlib and the extension is gz
    //

#ifndef GTO_SUPPORT_ZIP
    size_t i = m_inName.find(".gz", 0, 3);
    if (i == (strlen(filename) - 3))
    {
        fail( "this library was not compiled with zlib support" );
        return false;
    }
#endif

    //
    //  Figure out if this is a text GTO file
    //

#ifdef GTO_SUPPORT_ZIP
    m_gzfile = gzopen(filename, "rb");

    if (!m_gzfile)
    {
        //
        //  Try .gz version before giving up completely
        //

        std::string temp(filename);
        temp += ".gz";
        return open(temp.c_str());
    }

#else
    m_in = new ifstream(filename, ios::in|ios::binary);
    
    if ( !(*m_in) )
    {
        m_in = 0;
        fail( "stream failed to open" );
        return false;
    }
#endif

    m_needsClosing = true;
    m_error = false;

    readMagicNumber();

    if (m_header.magic == Header::MagicText ||
        m_header.magic == Header::CigamText)
    {
        close();
        m_in = new ifstream(filename, ios::in|ios::binary);

        if ( !(*m_in) )
        {
            delete m_in;
            m_in = 0;
            fail( "stream failed to open" );
            return false;
        }

        return open(*m_in, filename, TextOnly);
    }
    else
    {
        return readBinaryGTO();
    }
}

void
Reader::close()
{
    if (m_needsClosing) 
    {
        delete m_in;
        m_in = 0;
#ifdef GTO_SUPPORT_ZIP

        if (m_gzfile) 
        {
            gzclose(m_gzfile);
            m_gzfile = 0;
        }
#endif
    }

    //
    //  Clean everything up in case the Reader
    //  class is used for another file.
    //

    m_objects.clear();
    m_components.clear();
    m_properties.clear();
    m_strings.clear();
    m_stringMap.clear();
    m_buffer.clear();

    m_error        = false;
    m_inName       = "";
    m_needsClosing = false;
    m_swapped      = false;
    m_why          = "";
    m_linenum      = 0;
    m_charnum      = 0;
    memset(&m_header, 0, sizeof(m_header));
}

void Reader::header(const Header&) {}
Reader::Request Reader::object(const string&, const string&, unsigned int,
                    const ObjectInfo &) { return Request(true); }
Reader::Request Reader::component(const string&, const ComponentInfo &) { return Request(true); }
Reader::Request Reader::property(const string&, const PropertyInfo &) { return Request(true); }
void* Reader::data(const PropertyInfo&, size_t) { return 0; }
void Reader::dataRead(const PropertyInfo&) {}
void Reader::descriptionComplete() {}

Reader::Request 
Reader::component(const string& name, 
                  const string& interp, 
                  const ComponentInfo &c) 
{ 
    // make it backwards compatible with version 2
    return component(name, c);
}

Reader::Request 
Reader::property(const string& name, 
                 const string& interp,
                 const PropertyInfo &p) 
{
    // make it backwards compatible with version 2
    return property(name, p);
}

static void
swapWords(void *data, size_t size)
{
    struct bytes { char c[4]; };

    bytes* ip = reinterpret_cast<bytes*>(data);

    for (int i=0; i<size; i++)
    {
        bytes temp = ip[i];
        ip[i].c[0] = temp.c[3];
        ip[i].c[1] = temp.c[2];
        ip[i].c[2] = temp.c[1];
        ip[i].c[3] = temp.c[0];
    }
}

static void
swapShorts(void *data, size_t size)
{
    struct bytes { char c[2]; };

    bytes* ip = reinterpret_cast<bytes*>(data);

    for (int i=0; i<size; i++)
    {
        bytes temp = ip[i];
        ip[i].c[0] = temp.c[1];
        ip[i].c[1] = temp.c[0];
    }
}

void
Reader::readMagicNumber()
{
    m_header.magic = 0;
    read((char*)&m_header, sizeof(uint32));
}

void
Reader::readHeader()
{
    read((char*)&m_header + sizeof(uint32), 
         sizeof(Header) - sizeof(uint32));

    if (m_error) return;
    m_swapped = false;

    if (m_header.magic == Header::Cigam)
    {
        m_swapped = true;
        swapWords(&m_header, sizeof(m_header) / sizeof(int));
    }
    else if (m_header.magic != Header::Magic)
    {
        ostringstream str;
        str << "bad magic number (" << hex << m_header.magic << ")";
        fail( str.str() );
        return;
    }

    if (m_header.version != GTO_VERSION && m_header.version != 2)
    {
        fail( "version mismatch" );
        cerr << "ERROR: Gto::Reader: gto file version == " 
             << m_header.version << ", which is not readable by this version (v"
             << GTO_VERSION << ")\n";
        return;
    }

    header(m_header);
}

int
Reader::internString(const std::string& s)
{
    StringMap::iterator i = m_stringMap.find(s);

    if (i == m_stringMap.end())
    {
        m_strings.push_back(s);
        int index = m_strings.size() - 1;
        m_stringMap[s] = index;
        return index;
    }
    else
    {
        return i->second;
    }
}

void
Reader::readStringTable()
{
    for (int i=0; i < m_header.numStrings; i++)
    {
        string s;
        char c;

        for (get(c); c && notEOF(); get(c))
        {
            s.push_back(c);
        }

        m_strings.push_back(s);
    }
}

void
Reader::readObjects()
{
    int coffset = 0;

    for (int i=0; i < m_header.numObjects; i++)
    {
        ObjectInfo o;

        if (m_header.version == 2)
        {
            read((char*)&o, sizeof(ObjectHeader_v2));
            o.pad = 0;
        }
        else
        {
            read((char*)&o, sizeof(ObjectHeader));
        }

        if (m_error) return;

        if (m_swapped) swapWords(&o, sizeof(ObjectHeader) / sizeof(int));

        stringFromId(o.name);
        stringFromId(o.protocolName);
        o.coffset = coffset;
        coffset += o.numComponents;

        if (m_error)
        {
            o.requested = false;
            return;
        }
        else if (!(m_mode & RandomAccess))
        {
            Request r = object( stringFromId(o.name), 
                                stringFromId(o.protocolName), 
                                o.protocolVersion,
                                o);

            o.requested  = r.m_want;
            o.objectData = r.m_data;
        }

        m_objects.push_back(o);
    }
}


void
Reader::readComponents()
{
    int poffset = 0;

    for (Objects::iterator i = m_objects.begin();
         i != m_objects.end();
         ++i)
    {
        const ObjectInfo &o = *i;

        for (int q=0; q < o.numComponents; q++)
        {
            ComponentInfo c;

            if (m_header.version == 2)
            {
                read((char*)&c, sizeof(ComponentHeader_v2));
                c.pad = 0;
                c.interpretation = 0;
            }
            else
            {
                read((char*)&c, sizeof(ComponentHeader));
            }

            if (m_error) return;

            if (m_swapped) swapWords(&c, sizeof(ComponentHeader) / sizeof(int));

            c.object = &o;
            c.poffset = poffset;
            poffset += c.numProperties;

            if (o.requested && !(m_mode & RandomAccess))
            {
                stringFromId(c.name);       // sanity checks
                stringFromId(c.interpretation);
                if (m_error) return;

                Request r = component(stringFromId(c.name), 
                                      stringFromId(c.interpretation),
                                      c);

                c.requested = r.m_want;
                c.componentData = r.m_data;
            }
            else
            {
                c.requested = false;
            }

            m_components.push_back(c);
        }
    }
}

void
Reader::readProperties()
{
    for (Components::iterator i = m_components.begin();
         i != m_components.end();
         ++i)
    {
        const ComponentInfo &c = *i;

        for (int q=0; q < c.numProperties; q++)
        {
            PropertyInfo p;
            
            if (m_header.version == 2)
            {
                read((char*)&p, sizeof(PropertyHeader_v2));
                p.pad = 0;
                p.interpretation = 0;
            }
            else
            {
                read((char*)&p, sizeof(PropertyHeader));
            }

            if (m_error) return;

            if (m_swapped) 
            {
                swapWords(&p, sizeof(PropertyHeader) / sizeof(int));
            }
            
            p.component = &c;

            if (c.requested && !(m_mode & RandomAccess))
            {
                stringFromId(p.name);
                stringFromId(p.interpretation);
                if (m_error) return;

                Request r = property(stringFromId(p.name), 
                                     stringFromId(p.interpretation),
                                     p);

                p.requested = r.m_want;
                p.propertyData = r.m_data;
            }
            else
            {
                p.requested = false;
            }

            m_properties.push_back(p);
        }
    }
}

bool
Reader::accessObject(ObjectInfo& o)
{
    Request r = object(stringFromId(o.name), 
                       stringFromId(o.protocolName), 
                       o.protocolVersion,
                       o);
    
    o.requested  = r.m_want;
    o.objectData = r.m_data;

    if (o.requested)
    {
        for (int q=0; q < o.numComponents; q++)
        {
            assert( (o.coffset+q) < m_components.size() );
            ComponentInfo& c = m_components[o.coffset + q];
            const std::string &nme = stringFromId(c.name);
            Request        r = component(nme, c);
            c.requested      = r.m_want;
            c.componentData  = r.m_data;

            if (c.requested)
            {
                for (int j=0; j < c.numProperties; j++)
                {
                    PropertyInfo& p = m_properties[c.poffset + j];
                    Request       r = property(stringFromId(p.name), 
                                               stringFromId(p.interpretation),
                                               p);
                    p.requested     = r.m_want;
                    p.propertyData  = r.m_data;

                    if (p.requested)
                    {
                        seekTo(p.offset);
                        readProperty(p);
                    }
                }
            }
        }
    }

    return true;
}

bool
Reader::readTextGTO()
{
    m_header.magic = Header::MagicText;

    if (!::GTOParse(this))
    {
        fail("failed to parse text GTO");
        return false;
    }
    
    header(m_header);
    descriptionComplete();
    return true;
}

bool
Reader::readBinaryGTO()
{
    readHeader();           if (m_error) return false; 
    readStringTable();      if (m_error) return false;
    readObjects();          if (m_error) return false;
    readComponents();       if (m_error) return false;
    readProperties();       if (m_error) return false;
    descriptionComplete();

    if (m_mode & HeaderOnly)
    {
        return true;
    }

    Properties::iterator p = m_properties.begin();

    for (Components::iterator i = m_components.begin();
         i != m_components.end();
         ++i)
    {
        ComponentInfo &comp = *i;
            
        if (comp.flags & Gto::Transposed)
        {
            cerr << "ERROR: Transposed data for '"
                 << stringFromId( comp.object->name ) << "."
                 << stringFromId( comp.name ) 
                 << "' is currently unsupported." << endl;
            abort();
        }
        else
        {
            for (Properties::iterator e = p + comp.numProperties; p != e; ++p)
            {
                if (!readProperty(*p))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

bool
Reader::readProperty(PropertyInfo& prop)
{
    size_t num   = prop.size * prop.width;
    size_t bytes = num * dataSize(prop.type);
    char* buffer = 0;

    //
    //  Cache the offset pointer
    //

    prop.offset = tell();
    bool readok = false;

    if (prop.requested)
    {
        if (buffer = (char*)data(prop, bytes))
        {
            read(buffer, bytes);
            readok = true;
        }
        else
        {
            seekForward(bytes);
        }
    }
    else
    {
        seekForward(bytes);
    }

    if (m_error) return false;

    if (readok)
    {
        if (m_swapped)
        {
            switch (prop.type)
            {
              case Gto::Int: 
              case Gto::String:
              case Gto::Float: 
                  swapWords(buffer, num);
                  break;
                          
              case Gto::Short:
              case Gto::Half: 
                  swapShorts(buffer, num);
                  break;
                          
              case Gto::Double: 
                  swapWords(buffer, num * 2);
                  break;
                          
              case Gto::Byte:
              case Gto::Boolean: 
                  break;
            }
        }

        dataRead(prop);
    }


    return true;
}

bool
Reader::notEOF()
{
    if (m_in) 
    {
        return (*m_in);
    }
#ifdef GTO_SUPPORT_ZIP
    else if (m_gzfile)
    {
        return m_gzrval != -1;
    }
#endif

    return false;
}

void
Reader::read(char *buffer, size_t size)
{
    if (m_in)
    {
        m_in->read(buffer,size);

#ifdef HAVE_FULL_IOSTREAMS
        if (m_in->fail())
        {
            std::cerr << "ERROR: Gto::Reader: Failed to read gto file: '";
            std::cerr << m_inName << "': " << std::endl;
            memset( buffer, 0, size );
            fail( "stream fail" );
        }
#endif
    }

#ifdef GTO_SUPPORT_ZIP
    else if (m_gzfile)
    {
        if (gzread(m_gzfile, buffer, size) != size)
        {
            int zError = 0;
            std::cerr << "ERROR: Gto::Reader: Failed to read gto file: ";
            std::cerr << gzerror( m_gzfile, &zError );
            std::cerr << std::endl;
            memset( buffer, 0, size );
            fail( "gzread fail" );
        }
    }
#endif
}

void
Reader::get(char &c)
{
    if (m_in)
    {
        m_in->get(c);
    }
#ifdef GTO_SUPPORT_ZIP
    else if (m_gzfile)
    {
        m_gzrval = gzgetc(m_gzfile);
        c = char(m_gzrval);
    }
#endif
}

void Reader::fail( std::string why )
{
    m_error = true;
    m_why = why;
}

const std::string& Reader::stringFromId(unsigned int i)
{
    static std::string empty( "" );
    if (i < 0 || i >= m_strings.size())
    {
        std::cerr << "WARNING: Gto::Reader: Malformed gto file: ";
        std::cerr << "invalid string index" << std::endl;
        fail( "malformed file, invalid string index" );
        return empty;
    }

    return m_strings[i];
}

void Reader::seekForward(size_t bytes)
{
    if (m_in)
    {
#ifdef HAVE_FULL_IOSTREAMS
        m_in->seekg(bytes, ios_base::cur);
#else
        m_in->seekg(bytes, ios::cur);
#endif
    }
#ifdef GTO_SUPPORT_ZIP
    else
    {
        gzseek(m_gzfile, bytes, SEEK_CUR);
    }
#endif
}

void Reader::seekTo(size_t bytes)
{
    if (m_in)
    {
#ifdef HAVE_FULL_IOSTREAMS
        m_in->seekg(bytes, ios_base::beg);
#else
        m_in->seekg(bytes, ios::beg);
#endif
    }
#ifdef GTO_SUPPORT_ZIP
    else
    {
        gzseek(m_gzfile, bytes, SEEK_SET);
    }
#endif
}

int Reader::tell()
{
    if (m_in)
    {
        return m_in->tellg();
    }
#ifdef GTO_SUPPORT_ZIP
    else
    {
        return gztell(m_gzfile);
    }
#else
    else
    {
        fail("m_in undefined");
        return 0;
    }
#endif
}

//----------------------------------------------------------------------
//
//  Text parser entry points
//

void Reader::beginHeader(uint32 version)
{
    m_header.numStrings = 0;
    m_header.numObjects = 0;
    m_header.version    = version ? version : GTO_VERSION;
    m_header.flags      = 1;
}

void Reader::addObject(const ObjectInfo& info)
{
    //
    //  This function is requireed when the header info is read
    //  out-of-order (as with a text GTO file). In order to be backwards
    //  compatible with the info structures, we need to update pointers
    //  when STL allocates new memory.
    //
    
    if ((m_objects.size() > 0) && (m_objects.capacity() <= m_objects.size()))
    {
        const ObjectInfo* startAddress = &m_objects.front();
        m_objects.push_back(info);
        const ObjectInfo* newStartAddress = &m_objects.front();

        for (Components::iterator i = m_components.begin();
             i != m_components.end();
             ++i)
        {
            size_t offset = i->object - startAddress;
            i->object = newStartAddress + offset;
        }
    }
    else
    {
        m_objects.push_back(info);
    }
}

void Reader::addComponent(const ComponentInfo& info)
{
    //
    //  See addObject coments
    //
    
    if ((m_components.size() > 0) && (m_components.capacity() <= m_components.size()))
    {
        const ComponentInfo* startAddress = &m_components.front();
        m_components.push_back(info);
        const ComponentInfo* newStartAddress = &m_components.front();

        for (Properties::iterator i = m_properties.begin();
             i != m_properties.end();
             ++i)
        {
            size_t offset = i->component - startAddress;
            i->component = newStartAddress + offset;
        }
    }
    else
    {
        m_components.push_back(info);
    }
}

void Reader::beginObject(unsigned int name,
                         unsigned int proto,
                         unsigned int version)
{
    ObjectInfo info;
    info.name            = name;
    info.protocolName    = proto;
    info.protocolVersion = version;
    info.numComponents   = 0;
    info.pad             = 0;
    info.coffset         = 0;

    Request r = object(stringFromId(name),
                       stringFromId(proto),
                       version,
                       info);

    info.requested  = r.want();
    info.objectData = r.data();

    addObject(info);
}


void Reader::beginComponent(unsigned int name,
                            unsigned int interp)
{
    ComponentInfo info;
    info.name           = name;
    info.numProperties  = 0;
    info.flags          = 0;
    info.interpretation = interp;
    info.pad            = 0;
    info.poffset        = 0;
    info.object         = &m_objects.back();

    m_objects.back().numComponents++;

    if (info.object->requested)
    {
        Request r = component(stringFromId(name),
                              stringFromId(interp),
                              info);

        info.requested     = r.want();
        info.componentData = r.data();
    }
    else
    {
        info.requested     = false;
        info.componentData = 0;
    }

    addComponent(info);
}

void Reader::beginProperty(unsigned int name,
                           unsigned int interp,
                           unsigned int width,
                           unsigned int size,
                           DataType type)
{
    PropertyInfo info;
    info.name           = name;
    info.interpretation = interp;
    info.size           = 0;
    info.type           = type;
    info.width          = width;
    info.component      = &m_components.back();

    m_components.back().numProperties++;

    m_buffer.clear();

    m_currentType.type  = type;
    m_currentType.size  = size;
    m_currentType.width = width;

    if (info.component->requested)
    {
        Request r = property(stringFromId(name), 
                             stringFromId(interp),
                             info);

        info.requested    = r.want();
        info.propertyData = r.data();
    }
    else
    {
        info.requested    = false;
        info.propertyData = 0;
    }

    m_properties.push_back(info);
}

size_t Reader::numAtomicValuesInBuffer() const
{
    return m_buffer.size() / dataSize(m_currentType.type);
}

size_t Reader::numElementsInBuffer() const
{
    return numAtomicValuesInBuffer() / m_currentType.width;
}

void Reader::fillToSize(size_t size)
{
    size_t elementSize = dataSize(m_currentType.type) * m_currentType.width;
    ByteArray element(elementSize);

    memcpy(&element[0], &m_buffer[m_buffer.size() - elementSize], elementSize);

    while (numElementsInBuffer() != size)
    {
        copy(element.begin(), element.end(), back_inserter(m_buffer));
    }
}

void Reader::parseError(const char* msg)
{
    cerr << "ERROR: parsing GTO file \""
         << infileName()
         << "\" at line " << linenum()
         << ", char " << charnum()
         << " : " << msg
         << endl;
}

void Reader::parseWarning(const char* msg)
{
    cerr << "WARNING: parsing GTO file \""
         << infileName()
         << "\" at line " << linenum()
         << ", char " << charnum()
         << " : " << msg
         << endl;
}

void Reader::endProperty()
{
    //
    //  Give the data to the reader sub-class
    //

    PropertyInfo& info = m_properties.back();
    info.size = numElementsInBuffer();

    if (info.requested)
    {
        if (void* buffer = data(info, m_buffer.size()))
        {
            memcpy(buffer, &m_buffer.front(), m_buffer.size());
            dataRead(info);
        }
    }

    m_buffer.clear();
}

void Reader::endFile()
{
    m_header.numStrings = m_strings.size();
}

} // Gto
