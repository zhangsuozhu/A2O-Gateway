#ifndef HANDLERS_H
#define HANDLERS_H

#include <event2/http.h>
#include "types.h"

void handle_root(struct evhttp_request *req, void *arg);

#endif