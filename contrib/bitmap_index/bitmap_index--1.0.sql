\echo Use "CREATE EXTENSION bitmap_index" to load this file. \quit

CREATE OR REPLACE FUNCTION bmhandler(internal) RETURNS index_am_handler
AS '$libdir/bitmap_index', 'bmhandler'
LANGUAGE C;

CREATE ACCESS METHOD bitmap TYPE INDEX HANDLER bmhandler;

COMMENT ON ACCESS METHOD bitmap IS 'bitmap index access method';

CREATE OPERATOR CLASS int4_bitmap_ops
DEFAULT FOR TYPE int4 USING bitmap AS
    OPERATOR    1   <,
    OPERATOR    2   <=,
    OPERATOR    3   =,
    OPERATOR    4   >=,
    OPERATOR    5   >;
CREATE OR REPLACE FUNCTION create_bitmap_index(
    index_name text,
    table_name text,
    column_name text
) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE 'CREATE INDEX ' || quote_ident(index_name) ||
            ' ON ' || quote_ident(table_name) ||
            ' USING bitmap (' || quote_ident(column_name) || ')';
END;
$$;

-- CREATE OR REPLACE FUNCTION bitmap_index_exists(index_name text)
-- RETURNS boolean
-- LANGUAGE sql AS $$
--     SELECT EXISTS (
--         SELECT 1 FROM pg_index i
--         JOIN pg_class c ON c.oid = i.indexrelid
--         JOIN pg_am a ON a.oid = c.relam
--         WHERE c.relname = $1 AND a.amname = 'bitmap'
--     );
-- $$;