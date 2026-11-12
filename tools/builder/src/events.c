#include "events.h"

#include <stdlib.h>
#include <string.h>

static const char* const CONDITIONS[][2] = {
    {"actorValid", "actorId"},   {"itemValid", "itemId"},       {"selfSwitchValid", "selfSwitchCh"},
    {"switch1Valid", "switch1Id"}, {"switch2Valid", "switch2Id"}, {"variableValid", "variableId"},
};

static int falsy(const Node* n) {
    if (!n) return 1;
    switch (n->type) {
    case N_NULL:
    case N_FALSE: return 1;
    case N_NUM: return n->num == 0;
    case N_STR: return n->len == 0;
    default: return 0;
    }
}

static void drop_falsy(Node* obj) {
    if (!obj || obj->type != N_OBJ) return;
    for (size_t i = obj->count; i-- > 0;) {
        if (falsy(obj->items[i])) node_remove(obj, obj->keys[i]);
    }
}

static void to_command(Node* cmd) {
    if (!cmd || cmd->type != N_OBJ) return;
    double code = node_numv(node_get(cmd, "code"), 0), indent = node_numv(node_get(cmd, "indent"), 0);
    if (code < 0 || code > 0xFFFF || indent < 0 || indent > 0x7FFF) fatal("event command out of range: code %g indent %g", code, indent);
    Node* params = node_take(cmd, "parameters");
    for (size_t i = 0; i < cmd->count; i++) {
        node_free(cmd->items[i]);
        free(cmd->keys[i]);
    }
    free(cmd->items);
    free(cmd->keys);
    cmd->keys = NULL;
    cmd->items = NULL;
    cmd->count = cmd->cap = 0;
    cmd->type = N_CMD;
    cmd->num = (double)((uint32_t)code | (uint32_t)indent << 16);
    if (params && params->type == N_ARR) {
        cmd->items = params->items;
        cmd->count = params->count;
        cmd->cap = params->cap;
        params->items = NULL;
        params->count = params->cap = 0;
    }
    node_free(params);
}

static void compact_route(Node* route) {
    if (!route || route->type != N_OBJ) return;
    Node* list = node_get(route, "list");
    for (size_t i = 0; list && list->type == N_ARR && i < list->count; i++) to_command(list->items[i]);
    drop_falsy(route);
}

static void compact_list(Node* list) {
    if (!list || list->type != N_ARR) return;
    for (size_t i = 0; i < list->count; i++) {
        Node* cmd = list->items[i];
        int code = (int)node_numv(node_get(cmd, "code"), 0);
        Node* params = node_get(cmd, "parameters");
        if (code == 205) compact_route(node_at(params, 1));
        else if (code == 505) to_command(node_at(params, 0));
        to_command(cmd);
    }
}

static void compact_conditions(Node* page) {
    Node* c = node_get(page, "conditions");
    if (!c || c->type != N_OBJ) return;
    for (size_t k = 0; k < sizeof(CONDITIONS) / sizeof(CONDITIONS[0]); k++) {
        if (falsy(node_get(c, CONDITIONS[k][0]))) node_remove(c, CONDITIONS[k][1]);
    }
    if (falsy(node_get(c, "variableValid"))) node_remove(c, "variableValue");
    drop_falsy(c);
    if (!c->count) node_remove(page, "conditions");
}

static void compact_page(Node* page) {
    if (!page || page->type != N_OBJ) return;
    compact_conditions(page);
    drop_falsy(node_get(page, "image"));
    compact_route(node_get(page, "moveRoute"));
    compact_list(node_get(page, "list"));
    drop_falsy(page);
}

void events_compact_map(Node* map) {
    Node* events = node_get(map, "events");
    for (size_t e = 0; events && events->type == N_ARR && e < events->count; e++) {
        Node* ev = events->items[e];
        if (ev->type != N_OBJ) continue;
        Node* pages = node_get(ev, "pages");
        for (size_t p = 0; pages && pages->type == N_ARR && p < pages->count; p++) compact_page(pages->items[p]);
        drop_falsy(ev);
    }
}

void events_compact_common(Node* common_events) {
    for (size_t i = 0; common_events && common_events->type == N_ARR && i < common_events->count; i++) {
        Node* ce = common_events->items[i];
        if (ce->type != N_OBJ) continue;
        compact_list(node_get(ce, "list"));
        drop_falsy(ce);
    }
}
