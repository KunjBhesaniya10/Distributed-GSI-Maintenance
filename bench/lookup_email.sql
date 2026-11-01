-- Point lookup on the unique email GSI (one matching row).
-- pgbench -D rows=<n>; emails are user<id>@<domain>, domain chosen by id % 5.
\set id random(1, :rows)
\set d :id % 5
SELECT * FROM users
WHERE email = 'user' || :id::text || '@' ||       -- ::text so the planner folds it to a constant
      CASE :d WHEN 0 THEN 'example.com' WHEN 1 THEN 'mail.org' WHEN 2 THEN 'iitb.ac.in'
              WHEN 3 THEN 'corp.net' ELSE 'post.io' END;
