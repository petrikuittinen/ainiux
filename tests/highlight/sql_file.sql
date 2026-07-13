-- SQL syntax-highlighting fixture
CREATE TABLE articles (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    title TEXT NOT NULL,
    status VARCHAR(20) DEFAULT 'draft',
    metadata JSONB DEFAULT '{}'::jsonb,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);

WITH ranked AS (
    SELECT
        id,
        title,
        status,
        row_number() OVER (PARTITION BY status ORDER BY created_at DESC) AS position
    FROM articles
    WHERE metadata->>'visible' = 'true'
)
SELECT * FROM ranked WHERE position <= 10;

INSERT INTO articles (title, status)
VALUES ('Syntax highlighting', 'published')
ON CONFLICT (title) DO UPDATE SET status = EXCLUDED.status
RETURNING id;

DO $body$
BEGIN
    RAISE NOTICE 'Latest article count: %', (SELECT count(*) FROM articles);
END
$body$;

/* Multiline SQL comment. */

