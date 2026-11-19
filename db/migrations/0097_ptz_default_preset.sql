-- migrate:up
ALTER TABLE streams ADD COLUMN ptz_default_preset TEXT DEFAULT '';

-- migrate:down
ALTER TABLE streams DROP COLUMN ptz_default_preset;
