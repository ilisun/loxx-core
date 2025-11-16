//
//  LoxxRouterBridge.mm
//  LoxxRouter
//
//  Objective-C++ bridge implementation
//

#import "LoxxRouterBridge.h"

#include <memory>
#include <string>
#include <vector>

// Import C++ routing core
#include "routing_core/router.h"
#include "routing_core/profile.h"

// Error domain definition (declared in header)
NSErrorDomain const LoxxRouterErrorDomain = @"com.loxx.router";

@implementation LoxxRouterBridge {
    std::unique_ptr<routing_core::Router> _router;
}

- (nullable instancetype)initWithPath:(NSString *)path
                                 zoom:(NSInteger)zoom
                        cacheCapacity:(NSInteger)cacheCapacity
                                error:(NSError **)error {
    self = [super init];
    if (self) {
        try {
            routing_core::RouterOptions options;
            options.tileZoom = (int)zoom;
            options.tileCacheCapacity = (size_t)cacheCapacity;
            
            std::string dbPath = [path UTF8String];
            _router = std::make_unique<routing_core::Router>(dbPath, options);
            
        } catch (const std::exception& e) {
            if (error) {
                NSString *desc = [NSString stringWithUTF8String:e.what()];
                *error = [NSError errorWithDomain:LoxxRouterErrorDomain
                                             code:LoxxRouterErrorCodeInternalError
                                         userInfo:@{NSLocalizedDescriptionKey: desc}];
            }
            return nil;
        }
    }
    return self;
}

- (nullable NSDictionary *)routeFrom:(CLLocationCoordinate2D)start
                                  to:(CLLocationCoordinate2D)end
                             profile:(NSInteger)profile
                               error:(NSError **)error {
    if (!_router) {
        if (error) {
            *error = [NSError errorWithDomain:LoxxRouterErrorDomain
                                         code:LoxxRouterErrorCodeInternalError
                                     userInfo:@{NSLocalizedDescriptionKey: @"Router not initialized"}];
        }
        return nil;
    }
    
    try {
        // Convert to C++ types
        routing_core::Coord startCoord{start.latitude, start.longitude};
        routing_core::Coord endCoord{end.latitude, end.longitude};
        
        // Select profile
        routing_core::ProfileSettings prof = (profile == 0)
            ? routing_core::makeCarProfile()
            : routing_core::makeFootProfile();
        
        // Calculate route
        std::vector<routing_core::Coord> waypoints = {startCoord, endCoord};
        routing_core::RouteResult result = _router->route(prof, waypoints);
        
        // Check status
        if (result.status != routing_core::RouteStatus::OK) {
            if (error) {
                NSString *desc = @"Unknown error";
                LoxxRouterErrorCode code = LoxxRouterErrorCodeInternalError;
                
                switch (result.status) {
                    case routing_core::RouteStatus::NO_ROUTE:
                        desc = @"No route found";
                        code = LoxxRouterErrorCodeNoRoute;
                        break;
                    case routing_core::RouteStatus::NO_TILE:
                        desc = @"No tile data";
                        code = LoxxRouterErrorCodeNoTile;
                        break;
                    case routing_core::RouteStatus::DATA_ERROR:
                        desc = @"Data corrupted";
                        code = LoxxRouterErrorCodeDataCorrupted;
                        break;
                    default:
                        if (!result.error_message.empty()) {
                            desc = [NSString stringWithUTF8String:result.error_message.c_str()];
                        }
                        break;
                }
                
                *error = [NSError errorWithDomain:LoxxRouterErrorDomain
                                             code:code
                                         userInfo:@{NSLocalizedDescriptionKey: desc}];
            }
            return nil;
        }
        
        // Convert polyline to NSArray
        NSMutableArray *coordinates = [NSMutableArray arrayWithCapacity:result.polyline.size()];
        for (const auto& coord : result.polyline) {
            CLLocationCoordinate2D c = CLLocationCoordinate2DMake(coord.lat, coord.lon);
            NSValue *value = [NSValue valueWithBytes:&c objCType:@encode(CLLocationCoordinate2D)];
            [coordinates addObject:value];
        }
        
        // Return result dictionary
        return @{
            @"coordinates": coordinates,
            @"distance": @(result.distance_m),
            @"duration": @(result.duration_s)
        };
        
    } catch (const std::exception& e) {
        if (error) {
            NSString *desc = [NSString stringWithUTF8String:e.what()];
            *error = [NSError errorWithDomain:LoxxRouterErrorDomain
                                         code:LoxxRouterErrorCodeInternalError
                                     userInfo:@{NSLocalizedDescriptionKey: desc}];
        }
        return nil;
    }
}

@end

