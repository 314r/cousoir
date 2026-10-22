-- Saved searches, virtual libraries, grouped search terms and user
-- categories, as Calibre stores them, for the searches of the golden test.
INSERT OR REPLACE INTO preferences(key, val) VALUES ('saved_searches',
  '{"Mes favoris": "rating:>=4", "Cook": "author:cook", "Récents": "date:>2020", "Boucle": "search:boucle", "Imbriquée": "search:\"mes favoris\" and not tags:false"}');
INSERT OR REPLACE INTO preferences(key, val) VALUES ('virtual_libraries',
  '{"Français": "languages:fra", "Fantasy": "tags:fantasy or tags:\"=fiction.fantasy\"", "Vide": "id:<0", "Récursive": "vl:Récursive"}');
INSERT OR REPLACE INTO preferences(key, val) VALUES ('grouped_search_terms',
  '{"people": ["authors", "publisher"], "words": ["title", "comments", "tags"]}');
INSERT OR REPLACE INTO preferences(key, val) VALUES ('user_categories',
  '{"Favoris": [["Boris Vian", "authors", 0], ["Fantasy", "tags", 0], ["Roman", "tags", 0]], "Favoris.Sous": [["Glen Cook", "authors", 0]]}');
