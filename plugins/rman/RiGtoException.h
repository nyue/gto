//******************************************************************************
// Copyright (c) 2001-2002 Tweak Inc. All rights reserved.
//******************************************************************************

#ifndef _RiGtoException_h_
#define _RiGtoException_h_

#include <TwkExc/TwkExcException.h>
#include <stdlib.h>
#include <iostream>

namespace RiGto {

TWK_EXC_DECLARE( Exception, TwkExc::Exception, "RiGto::Exception: " );

#define TWK_FAKE_EXCEPTION( e, errorString )                \
        {                                                   \
            std::cerr << std::endl << "ERROR: RiGto "       \
                "Exception at line " << __LINE__ << " in "  \
                 << __FILE__ << ": " << errorString         \
                 << std::endl << std::endl ;                \
            if( ! getenv( "TWK_RI_CONTINUE_ON_ERROR" ) )    \
            {                                               \
                exit( 1 );                                  \
            }                                               \
        }

} // End namespace RiGto

#endif
