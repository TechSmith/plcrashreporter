//
//  CPPCrashHelper.m
//  CrashReporter
//
//  Created by Rory Hool on 3/21/24.
//

#import "CPPCrashHelper.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <typeinfo>

#define DESCRIPTION_BUFFER_LENGTH 1000

@implementation CPPCrashHelper

+(NSString*)retrieveExceptionInfo
{
    NSString* exceptionInfo = nil;
    
    const char* name = NULL;
    std::type_info* tinfo = __cxxabiv1::__cxa_current_exception_type();
    if ( tinfo != NULL )
    {
        name = tinfo->name();
    }
    
    // Ignore NSExceptions
    if ( name != NULL && strcmp(name, "NSException") != 0 )
    {
        NSString* nameString = [NSString stringWithFormat:@"name : %@", [NSString stringWithUTF8String:name]];
        exceptionInfo = nameString;
        
        char descriptionBuff[DESCRIPTION_BUFFER_LENGTH];
        const char* description = descriptionBuff;
        descriptionBuff[0] = 0;
        
        try
        {
            throw;
        }
        catch(std::exception& exc)
        {
            strncpy(descriptionBuff, exc.what(), sizeof(descriptionBuff));
        }
        NSString* descriptionString = [NSString stringWithFormat:@" description : %@", [NSString stringWithUTF8String:description]];
        exceptionInfo = [exceptionInfo stringByAppendingString:descriptionString];
    }
    
    return exceptionInfo;
}
@end
