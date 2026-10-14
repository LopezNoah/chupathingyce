/* External poses are stored beside native edits. Identity uses extension name
 * and asset resource, never process-specific pointers/registry positions. Load
 * waits until the provider has initialized its default geometry. */
static void forge_world_save(FILE *file)
{
    long provider, slot;
    boolean first = TRUE;
    for (provider = 0; provider < halo_extensions_count(); provider++) {
        struct halo_extension const *extension = halo_extensions_get(provider);
        struct halo_world_geometry const *geometry = extension->world_geometry;
        struct forge_target target = { _forge_target_world, (short)provider, 0 };
        if (!geometry || !geometry->edit_count || !geometry->edit_get) continue;
        for (slot = 0; slot < geometry->edit_count() && slot < FORGE_MAXIMUM_WORLD_EDITS; slot++) {
            struct forge_transform state;
            target.index = slot;
            if (!forge_world_get(&target, &state)) continue;
            fprintf(file, "%s\n    {\"type\": ", first ? "" : ",");
            forge_write_string(file, extension->name);
            fprintf(file, ", \"index\": %ld, \"tag\": ", slot);
            forge_write_string(file, geometry->edit_resource ? geometry->edit_resource() : "");
            fprintf(file, ", \"deleted\": %s, ", state.deleted ? "true" : "false");
            forge_write_transform(file, &state);
            fprintf(file, "}");
            first = FALSE;
        }
    }
}

static void forge_world_load(struct forge_json *json)
{
    forge_json_expect(json, '[');
    if (forge_json_take(json, ']')) return;
    do {
        struct forge_json_entry entry;
        long provider;
        forge_json_entry(json, &entry);
        if (json->error) return;
        if (entry.index < 0 || entry.index >= FORGE_MAXIMUM_WORLD_EDITS ||
            !forge_transform_valid(&entry.transform)) { json->error = TRUE; return; }
        for (provider = 0; provider < halo_extensions_count(); provider++) {
            struct halo_extension const *extension = halo_extensions_get(provider);
            struct halo_world_geometry const *geometry = extension->world_geometry;
            struct forge_world_loaded *loaded;
            if (strcmp(extension->name, entry.type) || !geometry || !geometry->edit_set ||
                !geometry->edit_count) continue;
            if (geometry->edit_resource && strcmp(geometry->edit_resource(), entry.tag)) {
                platform_log("forge: external %s asset changed; ignoring saved pose", extension->name);
                break;
            }
            if (forge.world_loaded_count >= FORGE_MAXIMUM_WORLD_EDITS) { json->error = TRUE; return; }
            loaded = &forge.world_loaded[forge.world_loaded_count++];
            loaded->target.kind = _forge_target_world;
            loaded->target.type = (short)provider;
            loaded->target.index = entry.index;
            loaded->state = entry.transform;
            loaded->applied = FALSE;
            break;
        }
    } while (!json->error && forge_json_take(json, ','));
    forge_json_expect(json, ']');
}

static void forge_world_apply_loaded(void)
{
    long row;
    for (row = 0; row < forge.world_loaded_count; row++) {
        struct forge_world_loaded *loaded = &forge.world_loaded[row];
        struct halo_world_geometry const *geometry = forge_world_geometry(&loaded->target);
        if (loaded->applied || !geometry || !geometry->edit_count || geometry->edit_count() == 0) continue;
        /* The writer saves contiguous slots in ascending order, deleted ones
         * included. Providers refuse holes and out-of-capacity slot indices. */
        loaded->applied = TRUE;
        if (forge_world_set(&loaded->target, &loaded->state)) {
            forge_nav_revision++;
            platform_log("forge: restored external %s slot %ld", halo_extensions_get(loaded->target.type)->name,
                loaded->target.index);
        } else platform_log("forge: external slot %ld rejected by provider", loaded->target.index);
    }
}
