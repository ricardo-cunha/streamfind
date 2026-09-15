use crate::{Error, ErrorCode, Json, Project, Result};

/// A table and the columns a domain module requires before execution.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TableRequirement {
    pub name: String,
    pub required_columns: Vec<String>,
}

/// Narrow table-access boundary exposed to domain modules.
pub struct ProjectTableStore<'a> {
    project: &'a Project,
}

impl<'a> ProjectTableStore<'a> {
    pub fn new(project: &'a Project) -> Self {
        Self { project }
    }

    pub fn has_table(&self, table_name: &str) -> Result<bool> {
        Ok(self
            .project
            .list_tables()?
            .iter()
            .any(|name| name == table_name))
    }

    pub fn require(&self, requirements: &[TableRequirement]) -> Result<()> {
        for requirement in requirements {
            if !self.has_table(&requirement.name)? {
                return Err(Error::new(
                    ErrorCode::SchemaMismatch,
                    format!("missing domain table: {}", requirement.name),
                ));
            }
            if requirement.required_columns.is_empty() {
                continue;
            }
            let rows = self
                .project
                .query_json(&format!("DESCRIBE \"{}\"", requirement.name))?;
            for column in &requirement.required_columns {
                if !rows
                    .as_array()
                    .into_iter()
                    .flatten()
                    .any(|row| row["column_name"].as_str() == Some(column))
                {
                    return Err(Error::new(
                        ErrorCode::SchemaMismatch,
                        format!(
                            "missing column {column} in domain table {}",
                            requirement.name
                        ),
                    ));
                }
            }
        }
        Ok(())
    }

    /// Validate every table contract generated from the semantic catalogue.
    pub fn require_manifest(&self, domain: &str) -> Result<()> {
        let manifest = crate::catalogue::table_manifest(domain)
            .map_err(|error| Error::new(ErrorCode::SchemaMismatch, error.to_string()))?;
        for (table_name, columns) in manifest {
            if !self.has_table(&table_name)? {
                return Err(Error::new(
                    ErrorCode::SchemaMismatch,
                    format!("missing domain table: {table_name}"),
                ));
            }
            let rows = self
                .project
                .query_json(&format!("DESCRIBE \"{}\"", table_name))?;
            for (column_name, semantic_type) in columns {
                let Some(row) = rows
                    .as_array()
                    .into_iter()
                    .flatten()
                    .find(|row| row["column_name"].as_str() == Some(column_name.as_str()))
                else {
                    return Err(Error::new(
                        ErrorCode::SchemaMismatch,
                        format!("missing column {column_name} in domain table {table_name}"),
                    ));
                };
                let expected = semantic_type.rsplit('#').next().unwrap_or(&semantic_type);
                let actual = row["column_type"]
                    .as_str()
                    .unwrap_or_default()
                    .to_ascii_uppercase();
                let compatible = match expected {
                    "boolean" => actual == "BOOLEAN",
                    "integer" => actual.contains("INT"),
                    "real" => actual == "DOUBLE" || actual == "FLOAT" || actual == "DECIMAL",
                    "string" => actual == "VARCHAR" || actual == "TEXT",
                    "timestamp" => actual.contains("TIMESTAMP"),
                    _ => true,
                };
                if !compatible {
                    return Err(Error::new(
                        ErrorCode::SchemaMismatch,
                        format!("incompatible type for {column_name} in {table_name}: expected {expected}, got {actual}"),
                    ));
                }
            }
        }
        Ok(())
    }

    pub fn ensure_table(&self, ddl: &str) -> Result<()> {
        self.project.execute_sql(ddl)
    }

    pub fn execute(&self, sql: &str) -> Result<()> {
        self.project.execute_sql(sql)
    }

    pub fn query(&self, sql: &str) -> Result<Json> {
        self.project.query_json(sql)
    }
}
