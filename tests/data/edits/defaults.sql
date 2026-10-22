-- The values tests/data/library-custom's custom columns give new books, as
-- Calibre's column editor stores them in each column's display.
UPDATE custom_columns SET display = json_set(display, '$.default_value', 'Sans note') WHERE label = 'note';
UPDATE custom_columns SET display = json_set(display, '$.default_value', 'Défaut, Autre') WHERE label = 'genre';
UPDATE custom_columns SET display = json_set(display, '$.default_value', 'Anne Onyme & Bob') WHERE label = 'people';
UPDATE custom_columns SET display = json_set(display, '$.default_value', json('true')) WHERE label = 'read';
UPDATE custom_columns SET display = json_set(display, '$.default_value', 6) WHERE label = 'myrating';
UPDATE custom_columns SET display = json_set(display, '$.default_value', 3) WHERE label = 'half';
UPDATE custom_columns SET display = json_set(display, '$.default_value', 100) WHERE label = 'pagecount';
UPDATE custom_columns SET display = json_set(display, '$.default_value', 9.5) WHERE label = 'price';
UPDATE custom_columns SET display = json_set(display, '$.default_value', '2020-06-15') WHERE label = 'started';
UPDATE custom_columns SET display = json_set(display, '$.default_value', '<p>À lire</p>') WHERE label = 'review';
UPDATE custom_columns SET display = json_set(display, '$.default_value', 'C') WHERE label = 'shelf';
UPDATE custom_columns SET display = json_set(display, '$.default_value', 'Défaut') WHERE label = 'saga';
