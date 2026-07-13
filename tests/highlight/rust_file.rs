#![allow(dead_code)]

use std::collections::HashMap;

#[derive(Debug, Clone, PartialEq)]
pub enum Status<T> {
    Ready(T),
    Failed { message: String },
}

pub trait Render {
    fn render(&self) -> String;
}

pub struct Article<'a, T>
where
    T: Render + Send + Sync,
{
    pub id: u64,
    pub title: &'a str,
    pub body: T,
    pub metadata: HashMap<String, String>,
}

impl<T: Render + Send + Sync> Article<'_, T> {
    pub async fn summary(&self) -> Result<String, &'static str> {
        let Some(author) = self.metadata.get("author") else {
            return Err("missing author");
        };

        let state = match self.id {
            0 => Status::Failed { message: String::from("invalid") },
            _ => Status::Ready(self.body.render()),
        };

        let raw = r##"A raw string with "quotes" and # symbols."##;
        Ok(format!("{author}: {state:?} {raw}"))
    }
}

/* Outer comment
   /* nested Rust comment */
   closes here. */

