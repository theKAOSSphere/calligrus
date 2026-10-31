import React, { useState } from "react";

interface Props {
  title: string;
  count?: number;
}

export function Panel({ title, count = 0 }: Props): JSX.Element {
  const [open, setOpen] = useState<boolean>(false);
  // toggle on click
  return (
    <section className="panel" onClick={() => setOpen(!open)}>
      <h2>{title}</h2>
      {open && <Badge value={count * 2} />}
    </section>
  );
}
